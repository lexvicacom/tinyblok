#!/usr/bin/env python3
# codegen: patchbay.edn -> main/rules.zig.
#   python tools/gen.py patchbay.edn main/rules.zig

from __future__ import annotations

import os
import re
import sys
import tempfile
from dataclasses import dataclass, field
from typing import Iterator

# --- tokenizer / parser -----------------------------------------------------

Node = (
    str | float | list
)  # symbol/string/keyword wrapped, number as float, list of Node


# Symbols and keywords are wrapped so we can tell them apart from strings.
class Sym(str):
    pass


class Kw(str):
    pass


class Str(str):
    pass


_TOKEN_RE = re.compile(
    r"""
      \s+                          # whitespace
    | ;[^\n]*                      # comment to EOL
    | (?P<lp>\()
    | (?P<rp>\))
    | "(?P<str>[^"]*)"             # string (no escapes — matches gen.zig)
    | :(?P<kw>[^\s()";]+)          # :keyword
    | (?P<atom>[^\s()";]+)         # number or symbol
    """,
    re.VERBOSE,
)


def tokenize(src: str) -> list:
    out = []
    pos = 0
    while pos < len(src):
        m = _TOKEN_RE.match(src, pos)
        if not m:
            raise SyntaxError(f"tokenizer stuck at offset {pos}: {src[pos:pos+20]!r}")
        pos = m.end()
        if m.group("lp"):
            out.append("(")
        elif m.group("rp"):
            out.append(")")
        elif m.group("str") is not None:
            out.append(Str(m.group("str")))
        elif m.group("kw"):
            out.append(Kw(m.group("kw")))
        elif (a := m.group("atom")) is not None:
            if _is_number(a):
                out.append(float(a))
            else:
                out.append(Sym(a))
    return out


def _is_number(s: str) -> bool:
    if not s:
        return False
    i = 1 if s[0] in "+-" else 0
    if i >= len(s):
        return False
    saw_digit = False
    for c in s[i:]:
        if c.isdigit():
            saw_digit = True
        elif c == ".":
            pass
        else:
            return False
    return saw_digit


def parse(tokens: list) -> list:
    it = iter(tokens)
    return list(_parse_all(it))


def _parse_all(it: Iterator) -> Iterator[Node]:
    for t in it:
        yield _parse_one(t, it)


def _parse_one(t, it: Iterator) -> Node:
    if t == "(":
        items = []
        for nxt in it:
            if nxt == ")":
                return items
            items.append(_parse_one(nxt, it))
        raise SyntaxError("unexpected EOF inside list")
    if t == ")":
        raise SyntaxError("unexpected ')'")
    return t


# --- emitter ----------------------------------------------------------------


@dataclass
class Slot:
    name: str
    kind: str  # deadband | squelch | moving_avg_tick | rising_edge | falling_edge | throttle_ms
    arg: float


@dataclass
class RuleEmit:
    filter: str
    body_zig: str


@dataclass
class Pump:
    subject: str
    from_sym: str
    type_: str
    hz: int


# Per-type wire-up. read_ret is the Zig type the C extern returns; pre is a
# transform applied before formatting (Zig expression with {x} placeholder for
# the read value); fmt is the printf format string passed to snprintf.
_TYPE_TABLE: dict[str, dict] = {
    "u32":      {"read_ret": "u32",   "pre": "{x}",                      "fmt": "%lu"},
    "i32":      {"read_ret": "c_int", "pre": "{x}",                      "fmt": "%d"},
    "f32":      {"read_ret": "f32",   "pre": "@as(f64, {x})",            "fmt": "%.7f"},
    "uptime-s": {"read_ret": "u64",   "pre": "@as(f64, @floatFromInt({x})) / 1_000_000.0", "fmt": "%.3f"},
}


@dataclass
class Emitter:
    slots: list[Slot] = field(default_factory=list)
    rules: list[RuleEmit] = field(default_factory=list)
    pumps: list[Pump] = field(default_factory=list)
    rule_idx: int = 0
    current_filter: str = ""

    def slot_name(self, prefix: str, idx: int) -> str:
        return f"rule{self.rule_idx}_{prefix}{idx}"

    def add_slot(self, kind: str, prefix: str, arg: float) -> str:
        name = self.slot_name(prefix, len(self.slots))
        self.slots.append(Slot(name=name, kind=kind, arg=arg))
        return name

    def add_pump(self, list_node: list) -> None:
        if len(list_node) < 2:
            raise SyntaxError("(pump SUBJECT :from FN :type T :hz N) needs subject")
        subject = _expect_str(list_node[1])
        kw = _kwargs(list_node[2:])
        from_sym = kw.get("from")
        type_ = kw.get("type")
        hz = kw.get("hz")
        if not isinstance(from_sym, Sym):
            raise SyntaxError(f"pump {subject!r}: :from must be a C symbol")
        if not isinstance(type_, Sym):
            raise SyntaxError(f"pump {subject!r}: :type must be a symbol")
        if str(type_) not in _TYPE_TABLE:
            raise SyntaxError(f"pump {subject!r}: unknown :type {type_}")
        if not isinstance(hz, float) or hz <= 0 or hz != int(hz):
            raise SyntaxError(f"pump {subject!r}: :hz must be a positive integer")
        self.pumps.append(Pump(subject=subject, from_sym=str(from_sym), type_=str(type_), hz=int(hz)))

    def emit_rule(self, list_node: list) -> None:
        if len(list_node) < 3:
            raise SyntaxError("(on FILTER BODY...) needs filter and body")
        filt = _expect_str(list_node[1])
        self.current_filter = filt
        body_parts: list[str] = []
        for form in list_node[2:]:
            self._emit_form(body_parts, form)
        self.rules.append(RuleEmit(filter=filt, body_zig="".join(body_parts)))
        self.rule_idx += 1

    def _emit_form(self, out: list[str], n: Node) -> None:
        lst = _expect_list(n)
        if not lst:
            return
        head = _expect_sym(lst[0])
        if head == "->":
            self._emit_thread(out, lst)
        elif head == "when":
            self._emit_when(out, lst)
        elif head in ("publish!", "publish"):
            self._emit_terminal_publish(out, lst, None)
        else:
            raise SyntaxError(f"unknown rule form: {head}")

    # (-> X op1 op2 ...) — last-arg threading.
    def _emit_thread(self, out: list[str], lst: list) -> None:
        if len(lst) < 2:
            raise SyntaxError("(-> X ops...) needs a value")
        ops = lst[2:]
        need_label = _thread_has_break(ops)
        out.append("    blk: {\n" if need_label else "    {\n")
        out.append("        const __v0: f64 = ")
        out.append(_value_expr(lst[1]))
        out.append(";\n")

        current_var = 0
        post_edge = False
        for i, op in enumerate(ops):
            last = i == len(ops) - 1
            op_list = _expect_list(op)
            if not op_list:
                raise SyntaxError("empty op")
            op_head = _expect_sym(op_list[0])

            if op_head in ("publish!", "publish"):
                if not last:
                    raise SyntaxError("publish! must be last in ->")
                vv = None if post_edge else current_var
                self._emit_terminal_publish(out, op_list, vv)
                continue

            next_var = current_var + 1
            out.append(f"        const __v{next_var}")
            self._emit_op(out, op_list, current_var)
            current_var = next_var
            if op_head in ("rising-edge", "falling-edge"):
                post_edge = True
        out.append("    }\n")

    def _emit_op(self, out: list[str], op_list: list, in_var: int) -> None:
        head = _expect_sym(op_list[0])

        match head:
            case "deadband":
                arg = _expect_num(op_list[1])
                slot = self.add_slot("deadband", "db", arg)
                out.append(
                    f": f64 = state.{slot}.update(__v{in_var}) orelse break :blk;\n"
                )
            case "squelch":
                slot = self.add_slot("squelch", "sq", 0)
                out.append(
                    f": f64 = state.{slot}.update(__v{in_var}) orelse break :blk;\n"
                )
            case "moving-avg":
                # (moving-avg N) tick window or (moving-avg :ms N) approximated at 10 Hz.
                first = op_list[1]
                if isinstance(first, Kw):
                    if first != "ms":
                        raise SyntaxError("moving-avg keyword must be :ms")
                    if len(op_list) < 3:
                        raise SyntaxError("moving-avg :ms needs N")
                    samples = _expect_num(op_list[2]) / 100.0
                else:
                    samples = _expect_num(first)
                slot = self.add_slot("moving_avg_tick", "ma", samples)
                out.append(f": f64 = state.{slot}.update(__v{in_var});\n")
            case "throttle":
                ms = _expect_kwd_num(op_list[1:], "ms")
                slot = self.add_slot("throttle_ms", "th", ms * 1000.0)
                out.append(
                    f": f64 = state.{slot}.update(__v{in_var}, tinyblok_uptime_us()) orelse break :blk;\n"
                )
            case "round":
                decimals = _expect_num(op_list[1])
                if decimals == 0:
                    out.append(f": f64 = @round(__v{in_var});\n")
                else:
                    scale = 10.0**decimals
                    out.append(
                        f": f64 = @round(__v{in_var} * {_zig_num(scale)}) / {_zig_num(scale)};\n"
                    )
            case "rising-edge":
                slot = self.add_slot("rising_edge", "re", 0)
                out.append(f": bool = state.{slot}.update(__v{in_var} != 0);\n")
                out.append(f"        if (!__v{in_var + 1}) break :blk;\n")
            case "falling-edge":
                slot = self.add_slot("falling_edge", "fe", 0)
                out.append(f": bool = state.{slot}.update(__v{in_var} != 0);\n")
                out.append(f"        if (!__v{in_var + 1}) break :blk;\n")
            case _:
                # Pass-through for unsupported ops (e.g. bar!) so codegen still completes.
                sys.stderr.write(f"gen: warning: skipping unsupported op '{head}'\n")
                out.append(f": f64 = __v{in_var};\n")
                out.append(f"        _ = __v{in_var + 1}; // skipped: ({head} ...)\n")

    def _emit_terminal_publish(
        self, out: list[str], op_list: list, value_var: int | None
    ) -> None:
        if len(op_list) < 2:
            raise SyntaxError("publish! needs target")
        target = op_list[1]
        if isinstance(target, Str):
            target_zig = f'"{target}"'
        elif isinstance(target, list):
            if len(target) < 2:
                raise SyntaxError("subject-append needs an arg")
            sub_head = _expect_sym(target[0])
            if sub_head != "subject-append":
                raise SyntaxError(f"unknown publish target: {sub_head}")
            suffix = _expect_str(target[1])
            target_zig = f'"{self.current_filter}.{suffix}"'
        else:
            raise SyntaxError("unknown publish target")

        if value_var is not None:
            out.append(f"        pb.emitFloat({target_zig}, __v{value_var}, 6);\n")
        else:
            out.append(f'        pb.emit({target_zig}, "1");\n')

    def _emit_when(self, out: list[str], lst: list) -> None:
        if len(lst) < 3:
            raise SyntaxError("(when COND BODY...) needs cond and body")
        out.append("    if (")
        out.append(_emit_cond(lst[1]))
        out.append(") {\n")
        for form in lst[2:]:
            self._emit_form(out, form)
        out.append("    }\n")

    def render(self) -> str:
        out: list[str] = []
        out.append(
            "// AUTO-GENERATED by tools/gen.py — do not edit by hand.\n"
            "// Regenerate after editing patchbay.edn:\n"
            "//   make gen\n"
            "//\n"
            'const pb = @import("patchbay.zig");\n'
            "\n"
            "extern fn snprintf(buf: [*]u8, len: usize, fmt: [*:0]const u8, ...) c_int;\n"
            "extern fn vTaskDelay(ticks: u32) void;\n"
            "extern fn tinyblok_uptime_us() u64;\n"
            "extern fn strtod(nptr: [*:0]const u8, endptr: ?*[*:0]const u8) f64;\n"
        )
        # Externs declared by pumps. Skip duplicates (a pump :from may collide
        # with the always-emitted tinyblok_uptime_us above).
        always = {"tinyblok_uptime_us"}
        for p in self.pumps:
            if p.from_sym in always:
                continue
            ret = _TYPE_TABLE[p.type_]["read_ret"]
            out.append(f"extern fn {p.from_sym}() {ret};\n")
            always.add(p.from_sym)
        out.append("\n")
        out.append("const State = struct {\n")

        for slot in self.slots:
            match slot.kind:
                case "deadband":
                    out.append(
                        f"    {slot.name}: pb.Deadband = .{{ .threshold = {_zig_num(slot.arg)} }},\n"
                    )
                case "squelch":
                    out.append(f"    {slot.name}: pb.Squelch = .{{}},\n")
                case "moving_avg_tick":
                    out.append(
                        f"    {slot.name}: pb.MovingAvg({int(slot.arg)}) = .{{}},\n"
                    )
                case "rising_edge":
                    out.append(f"    {slot.name}: pb.RisingEdge = .{{}},\n")
                case "falling_edge":
                    out.append(f"    {slot.name}: pb.FallingEdge = .{{}},\n")
                case "throttle_ms":
                    out.append(
                        f"    {slot.name}: pb.Throttle = .{{ .interval_us = {int(slot.arg)} }},\n"
                    )

        out.append("};\n" "\n" "var state: State = .{};\n" "\n")

        # Group rules by filter, preserving first-seen order.
        seen: list[str] = []
        for r in self.rules:
            if r.filter not in seen:
                seen.append(r.filter)
        for filt in seen:
            fn_name = _filter_to_fn_name(filt)
            out.append(f"fn {fn_name}(payload_float: f64) void {{\n")
            for r in self.rules:
                if r.filter == filt:
                    out.append(r.body_zig)
            out.append("}\n\n")

        out.append(
            "fn eql(a: []const u8, b: []const u8) bool {\n"
            "    if (a.len != b.len) return false;\n"
            "    var i: usize = 0;\n"
            "    while (i < a.len) : (i += 1) if (a[i] != b[i]) return false;\n"
            "    return true;\n"
            "}\n"
            "\n"
            "pub fn dispatch(subject: []const u8, payload: []const u8) void {\n"
            "    var subj_buf: [64]u8 = undefined;\n"
            "    if (subject.len + 1 > subj_buf.len) return;\n"
            "    @memcpy(subj_buf[0..subject.len], subject);\n"
            "    subj_buf[subject.len] = 0;\n"
            "    const subj_z: [*:0]const u8 = @ptrCast(&subj_buf);\n"
            "\n"
            "    var pl_buf: [64]u8 = undefined;\n"
            "    if (payload.len + 1 > pl_buf.len) return;\n"
            "    @memcpy(pl_buf[0..payload.len], payload);\n"
            "    pl_buf[payload.len] = 0;\n"
            "    const pl_z: [*:0]const u8 = @ptrCast(&pl_buf);\n"
            "\n"
            "    pb.emit(subj_z, payload);\n"
            "    const v: f64 = strtod(pl_z, null);\n"
            "\n"
        )

        for i, filt in enumerate(seen):
            fn_name = _filter_to_fn_name(filt)
            kw = "if" if i == 0 else "} else if"
            out.append(f'    {kw} (eql(subject, "{filt}")) {{\n        {fn_name}(v);\n')
        out.append("    }\n}\n")

        if self.pumps:
            out.append(self._render_collect())
        return "".join(out)

    def _render_collect(self) -> str:
        tick_hz = max(p.hz for p in self.pumps)
        for p in self.pumps:
            if tick_hz % p.hz != 0:
                raise SyntaxError(
                    f"pump {p.subject!r}: :hz {p.hz} does not divide tick rate {tick_hz}"
                )
        out: list[str] = []
        out.append(
            "\n"
            f"pub const tick_hz: u32 = {tick_hz};\n"
            f"pub const tick_ms: u32 = {1000 // tick_hz};\n"
            "\n"
            "fn dispatchFmt(subject: []const u8, comptime fmt: [*:0]const u8, args: anytype) void {\n"
            "    var buf: [32]u8 = undefined;\n"
            "    const n = @call(.auto, snprintf, .{ &buf, buf.len, fmt } ++ args);\n"
            "    if (n <= 0) return;\n"
            "    const len: usize = @min(@as(usize, @intCast(n)), buf.len - 1);\n"
            "    dispatch(subject, buf[0..len]);\n"
            "}\n"
            "\n"
            "pub fn collect() void {\n"
            "    const State_ = struct { var counter: u32 = 0; };\n"
            "    const c = State_.counter;\n"
        )
        for p in self.pumps:
            every = tick_hz // p.hz
            entry = _TYPE_TABLE[p.type_]
            pre = entry["pre"].replace("{x}", f"{p.from_sym}()")
            fmt = entry["fmt"]
            guard = "" if every == 1 else f"if (c % {every} == 0) "
            out.append(f'    {guard}dispatchFmt("{p.subject}", "{fmt}", .{{{pre}}});\n')
        out.append(
            "    State_.counter = c +% 1;\n"
            "}\n"
        )
        return "".join(out)


# --- helpers ----------------------------------------------------------------


def _thread_has_break(ops: list) -> bool:
    for op in ops:
        if not isinstance(op, list) or not op:
            continue
        head = op[0]
        if isinstance(head, Sym) and head in (
            "deadband",
            "squelch",
            "throttle",
            "rising-edge",
            "falling-edge",
        ):
            return True
    return False


def _value_expr(n: Node) -> str:
    if isinstance(n, Sym):
        if n in ("payload-float", "payload"):
            return "payload_float"
        raise SyntaxError(f"unknown symbol: {n}")
    if isinstance(n, float):
        return f"@as(f64, {_zig_num(n)})"
    raise SyntaxError("expected number or symbol")


def _emit_cond(n: Node) -> str:
    if not isinstance(n, list):
        raise SyntaxError("cond must be a list")
    if not n:
        raise SyntaxError("empty cond")
    head = _expect_sym(n[0])
    binops = {"<": "<", ">": ">", "<=": "<=", ">=": ">=", "=": "=="}
    if head in binops:
        if len(n) != 3:
            raise SyntaxError(f"{head} needs two args")
        return f"({_value_expr(n[1])} {binops[head]} {_value_expr(n[2])})"
    if head == "not":
        return f"!({_emit_cond(n[1])})"
    if head == "and":
        return "(" + " and ".join(_emit_cond(s) for s in n[1:]) + ")"
    if head == "or":
        return "(" + " or ".join(_emit_cond(s) for s in n[1:]) + ")"
    raise SyntaxError(f"unknown cond form: {head}")


def _zig_num(x: float) -> str:
    # Match Zig's {d} formatting: integers print with no fractional part, floats keep theirs.
    if x == int(x):
        return str(int(x))
    return repr(x)


def _filter_to_fn_name(filt: str) -> str:
    prefix = "tinyblok."
    tail = filt[len(prefix) :] if filt.startswith(prefix) else filt
    out = ["on"]
    upcase = True
    for c in tail:
        if c in ".-":
            out.append("_")
            upcase = False
        elif upcase:
            out.append(c.upper())
            upcase = False
        else:
            out.append(c)
    return "".join(out)


def _expect_list(n) -> list:
    if not isinstance(n, list):
        raise SyntaxError("expected list")
    return n


def _expect_str(n) -> str:
    if not isinstance(n, Str):
        raise SyntaxError("expected string")
    return str(n)


def _expect_num(n) -> float:
    if not isinstance(n, float):
        raise SyntaxError("expected number")
    return n


def _expect_sym(n) -> str:
    if not isinstance(n, Sym):
        raise SyntaxError("expected symbol")
    return str(n)


def _kwargs(args: list) -> dict[str, object]:
    if len(args) % 2 != 0:
        raise SyntaxError("expected :key value pairs")
    out: dict[str, object] = {}
    for i in range(0, len(args), 2):
        if not isinstance(args[i], Kw):
            raise SyntaxError(f"expected keyword, got {args[i]!r}")
        out[str(args[i])] = args[i + 1]
    return out


def _expect_kwd_num(args: list, want: str) -> float:
    if len(args) < 2:
        raise SyntaxError("expected keyword + number")
    if not isinstance(args[0], Kw):
        raise SyntaxError("expected keyword")
    if args[0] != want:
        raise SyntaxError(f"expected :{want}, got :{args[0]}")
    return _expect_num(args[1])


# --- main -------------------------------------------------------------------


def main(argv: list[str]) -> int:
    if len(argv) != 3:
        sys.stderr.write("usage: gen.py <patchbay.edn> <out rules.zig>\n")
        return 2
    in_path, out_path = argv[1], argv[2]

    with open(in_path, "r", encoding="utf-8") as f:
        src = f.read()

    forms = parse(tokenize(src))

    em = Emitter()
    for form in forms:
        if not isinstance(form, list) or len(form) < 2:
            continue
        head = form[0]
        if not isinstance(head, Sym):
            continue
        if head == "on":
            em.emit_rule(form)
        elif head == "pump":
            em.add_pump(form)

    rendered = em.render()

    out_dir = os.path.dirname(os.path.abspath(out_path)) or "."
    fd, tmp_path = tempfile.mkstemp(prefix=".gen.", dir=out_dir)
    try:
        with os.fdopen(fd, "w", encoding="utf-8") as tmp:
            tmp.write(rendered)
        os.replace(tmp_path, out_path)
    except Exception:
        try:
            os.unlink(tmp_path)
        except OSError:
            pass
        raise

    sys.stderr.write(
        f"wrote {out_path} ({len(rendered)} bytes, {len(em.rules)} rules, {len(em.slots)} state slots)\n"
    )
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
