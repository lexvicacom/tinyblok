#!/usr/bin/env python3
# codegen: patchbay.edn -> main/zig/rules.zig.
#   python tools/gen.py patchbay.edn main/zig/rules.zig

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
    kind: str  # deadband | squelch | moving_avg_tick | rising_edge | falling_edge | throttle_ms | bar_tick | bar_time | sample | debounce
    arg: float
    # Clock slots: target subject or subject prefix for generated emits.
    subject: str = ""


@dataclass
class RuleEmit:
    filter: str
    body_zig: str
    reentrant: bool = False


@dataclass
class ReqEmit:
    subject: str
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
    reqs: list[ReqEmit] = field(default_factory=list)
    pumps: list[Pump] = field(default_factory=list)
    rule_idx: int = 0
    current_filter: str = ""
    current_reentrant: bool = False
    current_request: bool = False

    def slot_name(self, prefix: str, idx: int) -> str:
        return f"rule{self.rule_idx}_{prefix}{idx}"

    def add_slot(self, kind: str, prefix: str, arg: float, subject: str = "") -> str:
        name = self.slot_name(prefix, len(self.slots))
        self.slots.append(Slot(name=name, kind=kind, arg=arg, subject=subject))
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
        self.current_request = False
        # Optional :reentrant true between filter and body. Mirrors monoblok's
        # patchbay; default-off so a publish! doesn't accidentally re-enter
        # rule eval and surprise an existing rule set.
        body_start = 2
        reentrant = False
        while body_start < len(list_node) and isinstance(list_node[body_start], Kw):
            kw = str(list_node[body_start])
            if body_start + 1 >= len(list_node):
                raise SyntaxError(f"(on ...) keyword :{kw} missing value")
            val = list_node[body_start + 1]
            if kw == "reentrant":
                if not isinstance(val, Sym) or str(val) not in ("true", "false"):
                    raise SyntaxError(":reentrant must be true or false")
                reentrant = (str(val) == "true")
            else:
                raise SyntaxError(f"(on ...) unknown keyword :{kw}")
            body_start += 2
        if body_start >= len(list_node):
            raise SyntaxError("(on FILTER ... BODY) missing body")
        self.current_reentrant = reentrant
        body_parts: list[str] = []
        for form in list_node[body_start:]:
            self._emit_form(body_parts, form)
        self.rules.append(RuleEmit(filter=filt, body_zig="".join(body_parts), reentrant=reentrant))
        self.rule_idx += 1

    def emit_request(self, list_node: list) -> None:
        if len(list_node) < 3:
            raise SyntaxError("(on-req SUBJECT BODY...) needs subject and body")
        subject = _expect_str(list_node[1])
        self.current_filter = subject
        self.current_reentrant = False
        self.current_request = True
        body_parts: list[str] = []
        for form in list_node[2:]:
            self._emit_form(body_parts, form)
        self.reqs.append(ReqEmit(subject=subject, body_zig="".join(body_parts)))
        self.current_request = False
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
            # Bare top-level publish republishes the original payload bytes.
            # Inside `->`, value_var carries the transformed float instead.
            self._emit_terminal_publish(out, lst, None, raw_payload=True)
        elif head in ("reply!", "reply"):
            self._emit_terminal_reply(out, lst, None, raw_payload=True)
        elif head in ("count!", "count"):
            self._emit_terminal_count(out, lst, value_var=None)
        elif head == "sample!":
            self._emit_terminal_clock_emit(out, lst, "sample", value_var=None, raw_payload=True)
        elif head == "debounce!":
            self._emit_terminal_clock_emit(out, lst, "debounce", value_var=None, raw_payload=True)
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
                self._emit_terminal_publish(out, op_list, vv, raw_payload=False)
                continue

            if op_head in ("reply!", "reply"):
                if not last:
                    raise SyntaxError("reply! must be last in ->")
                vv = None if post_edge else current_var
                self._emit_terminal_reply(out, op_list, vv, raw_payload=False)
                continue

            if op_head in ("bar!", "bar"):
                if not last:
                    raise SyntaxError("bar! must be last in ->")
                if post_edge:
                    raise SyntaxError("bar! cannot follow rising-edge / falling-edge")
                self._emit_terminal_bar(out, op_list, current_var)
                continue

            if op_head in ("count!", "count"):
                if not last:
                    raise SyntaxError("count! must be last in ->")
                self._emit_terminal_count(out, op_list, current_var)
                continue

            if op_head in ("sample!", "debounce!"):
                if not last:
                    raise SyntaxError(f"{op_head} must be last in ->")
                if post_edge:
                    raise SyntaxError(f"{op_head} cannot follow rising-edge / falling-edge")
                kind = "sample" if op_head == "sample!" else "debounce"
                self._emit_terminal_clock_emit(out, op_list, kind, current_var, raw_payload=False)
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
            case "moving-avg" | "moving-sum" | "moving-max" | "moving-min":
                # (moving-* N) tick window or (moving-* :ms N) approximated at 10 Hz.
                first = op_list[1]
                if isinstance(first, Kw):
                    if first != "ms":
                        raise SyntaxError(f"{head} keyword must be :ms")
                    if len(op_list) < 3:
                        raise SyntaxError(f"{head} :ms needs N")
                    samples = _expect_num(op_list[2]) / 100.0
                else:
                    samples = _expect_num(first)
                kind, prefix = {
                    "moving-avg": ("moving_avg_tick", "ma"),
                    "moving-sum": ("moving_sum_tick", "ms"),
                    "moving-max": ("moving_max_tick", "mx"),
                    "moving-min": ("moving_min_tick", "mn"),
                }[head]
                slot = self.add_slot(kind, prefix, samples)
                out.append(f": f64 = state.{slot}.update(__v{in_var});\n")
            case "quantize":
                step = _expect_num(op_list[1])
                if step == 0:
                    raise SyntaxError("quantize STEP must be non-zero")
                out.append(f": f64 = pb.quantize({_zig_num(step)}, __v{in_var});\n")
            case "clamp":
                lo = _expect_num(op_list[1])
                hi = _expect_num(op_list[2])
                if lo > hi:
                    raise SyntaxError("clamp LO must be <= HI")
                out.append(f": f64 = pb.clamp({_zig_num(lo)}, {_zig_num(hi)}, __v{in_var});\n")
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
                raise SyntaxError(f"unsupported Tinyblok patchbay op: {head}")

    def _emit_terminal_publish(
        self, out: list[str], op_list: list, value_var: int | None,
        raw_payload: bool = False,
    ) -> None:
        if len(op_list) < 2:
            raise SyntaxError("publish! needs target")
        target_zig = self._target_zig(op_list[1])

        # Three cases for what gets sent:
        #   value_var: float result of a `->` chain (formatted %.6f)
        #   raw_payload: original payload bytes from a bare top-level (publish!)
        #   neither: post-edge marker "1" (rising/falling-edge gates discard the value)
        if raw_payload:
            payload_zig = "payload_raw"
        else:
            payload_zig = None  # signals "use float var or '1' below"

        if self.current_reentrant:
            if value_var is not None:
                out.append(
                    f"        pb.emitReentrantFloat({target_zig}, __v{value_var}, depth, &dispatchInternal);\n"
                )
            elif payload_zig is not None:
                out.append(
                    f"        pb.emitReentrant({target_zig}, {payload_zig}, depth, &dispatchInternal);\n"
                )
            else:
                out.append(
                    f'        pb.emitReentrant({target_zig}, "1", depth, &dispatchInternal);\n'
                )
        else:
            if value_var is not None:
                out.append(f"        pb.emitFloat({target_zig}, __v{value_var}, 6);\n")
            elif payload_zig is not None:
                out.append(f"        pb.emit({target_zig}, {payload_zig});\n")
            else:
                out.append(f'        pb.emit({target_zig}, "1");\n')

    def _emit_terminal_reply(
        self, out: list[str], op_list: list, value_var: int | None,
        raw_payload: bool = False,
    ) -> None:
        if not self.current_request:
            raise SyntaxError("reply! is only valid inside on-req")

        # Three cases for what gets sent:
        #   value_var: float result of a `->` chain
        #   raw_payload: original payload bytes, or explicit literal/value
        #   neither: post-edge marker "1"
        if value_var is not None:
            if len(op_list) != 1:
                raise SyntaxError("threaded reply! stores the threaded value; omit VALUE")
            out.append(f"        pb.replyFloat(reply_subject, __v{value_var}, 6);\n")
            return

        if raw_payload:
            if len(op_list) == 1:
                out.append("        pb.reply(reply_subject, payload_raw);\n")
                return
            if len(op_list) != 2:
                raise SyntaxError("reply! takes at most one payload value")
            value = op_list[1]
            if isinstance(value, Str):
                out.append(f'        pb.reply(reply_subject, "{value}");\n')
                return
            if isinstance(value, Sym) and value == "payload":
                out.append("        pb.reply(reply_subject, payload_raw);\n")
                return
            if isinstance(value, Sym) and value == "payload-float":
                out.append("        pb.replyFloat(reply_subject, payload_float, 6);\n")
                return
            if isinstance(value, float):
                out.append(f"        pb.replyFloat(reply_subject, {_zig_num(value)}, 6);\n")
                return
            raise SyntaxError("reply! value must be a string, number, payload, or payload-float")

        if len(op_list) != 1:
            raise SyntaxError("reply! after an edge gate cannot take VALUE")
        out.append('        pb.reply(reply_subject, "1");\n')

    def _target_zig(self, target: Node) -> str:
        if isinstance(target, Str):
            return f'"{target}"'
        if isinstance(target, list):
            if len(target) < 2:
                raise SyntaxError("subject-append needs an arg")
            sub_head = _expect_sym(target[0])
            if sub_head != "subject-append":
                raise SyntaxError(f"unknown publish target: {sub_head}")
            suffix = _expect_str(target[1])
            return f'"{self.current_filter}.{suffix}"'
        raise SyntaxError("unknown publish target")

    def _emit_terminal_clock_emit(
        self,
        out: list[str],
        op_list: list,
        kind: str,
        value_var: int | None,
        raw_payload: bool,
    ) -> None:
        if len(op_list) < 4:
            raise SyntaxError(f"{kind}! needs :ms N SUBJECT")
        if not isinstance(op_list[1], Kw) or op_list[1] != "ms":
            raise SyntaxError(f"{kind}! window must be :ms N")
        period_ms = int(_expect_num(op_list[2]))
        if period_ms <= 0:
            raise SyntaxError(f"{kind}! :ms N must be positive")
        target_zig = self._target_zig(op_list[3])
        slot = self.add_slot(kind, "sa" if kind == "sample" else "dbn", float(period_ms), subject=target_zig.strip('"'))
        slot_id = self._clock_slot_id(slot)
        update = "updateSample" if kind == "sample" else "updateDebounce"
        if raw_payload:
            if len(op_list) == 4:
                out.append(
                    f"        if (state.{slot}.{update}(tinyblok_now_ms(), payload_raw)) |__dl| {{\n"
                    f"            tinyblok_clock_arm({slot_id}, deadlineUsFromNow(__dl));\n"
                    f"        }}\n"
                )
            elif len(op_list) == 5:
                value = op_list[4]
                if isinstance(value, Sym) and value == "payload":
                    out.append(
                        f"        if (state.{slot}.{update}(tinyblok_now_ms(), payload_raw)) |__dl| {{\n"
                        f"            tinyblok_clock_arm({slot_id}, deadlineUsFromNow(__dl));\n"
                        f"        }}\n"
                    )
                else:
                    out.append(
                        f"        if (state.{slot}.{update}Float(tinyblok_now_ms(), {_value_expr(value)})) |__dl| {{\n"
                        f"            tinyblok_clock_arm({slot_id}, deadlineUsFromNow(__dl));\n"
                        f"        }}\n"
                    )
            else:
                raise SyntaxError(f"{kind}! takes :ms N SUBJECT [VALUE]")
        else:
            if len(op_list) != 4:
                raise SyntaxError(f"threaded {kind}! stores the threaded value; omit VALUE")
            out.append(
                f"        if (state.{slot}.{update}Float(tinyblok_now_ms(), __v{value_var})) |__dl| {{\n"
                f"            tinyblok_clock_arm({slot_id}, deadlineUsFromNow(__dl));\n"
                f"        }}\n"
            )

    def _emit_terminal_bar(self, out: list[str], op_list: list, value_var: int) -> None:
        if len(op_list) < 2:
            raise SyntaxError("bar! needs window spec")
        first = op_list[1]
        subject_prefix = self.current_filter
        if isinstance(first, Kw):
            if first != "ms":
                raise SyntaxError("bar! keyword must be :ms")
            if len(op_list) < 3:
                raise SyntaxError("bar! :ms needs N")
            window_ms = int(_expect_num(op_list[2]))
            if window_ms <= 0:
                raise SyntaxError("bar! :ms N must be positive")
            slot = self.add_slot("bar_time", "bt", float(window_ms), subject=subject_prefix)
        else:
            cap = int(_expect_num(first))
            if cap <= 0:
                raise SyntaxError("bar! N must be positive")
            slot = self.add_slot("bar_tick", "bk", float(cap), subject=subject_prefix)

        last_slot = self.slots[-1]
        is_time = last_slot.kind == "bar_time"
        update_call = (
            f"state.{slot}.timeUpdate(tinyblok_now_ms(), __v{value_var})"
            if is_time
            else f"state.{slot}.tickUpdate(__v{value_var})"
        )
        out.append(f"        if ({update_call}) |__c| {{\n")
        for f in ("open", "high", "low", "close"):
            out.append(
                f'            pb.emitFloat("{subject_prefix}.bar.{f}", __c.{f}, 6);\n'
            )
        out.append("        }\n")
        # Time bars: after every push, (re)arm the per-slot one-shot timer at
        # `nextDeadlineMs`. This lands the close at the exact window boundary
        # even if the feed goes quiet, and replaces the periodic walker.
        if is_time:
            slot_id = self._bar_time_slot_id(slot)
            out.append(
                f"        if (state.{slot}.nextDeadlineMs()) |__dl| {{\n"
                f"            tinyblok_clock_arm({slot_id}, deadlineUsFromNow(__dl));\n"
                f"        }}\n"
            )

    def _bar_time_slot_id(self, slot_name: str) -> int:
        return self._clock_slot_id(slot_name)

    def _clock_slot_id(self, slot_name: str) -> int:
        """Return the dense u32 slot id for a clock slot, in declaration
        order. Used to index the per-slot clock table on the C side."""
        idx = 0
        for s in self.slots:
            if s.kind not in ("bar_time", "sample", "debounce"):
                continue
            if s.name == slot_name:
                return idx
            idx += 1
        raise KeyError(f"clock slot {slot_name} not found")

    def _emit_terminal_count(self, out: list[str], op_list: list, value_var: int | None) -> None:
        if len(op_list) > 1:
            raise SyntaxError("count! takes no args (use (when COND ... (count!)) for conditional)")
        _ = value_var
        slot = self.add_slot("count", "ct", 0, subject=self.current_filter)
        # update(true) always returns the new total; non-null guaranteed.
        out.append(f"        if (state.{slot}.update(true)) |__n| {{\n")
        out.append(f'            pb.emitInt("{self.current_filter}.count", @intCast(__n));\n')
        out.append("        }\n")

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
            "extern fn tinyblok_now_ms() i64;\n"
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
                case "moving_sum_tick":
                    out.append(
                        f"    {slot.name}: pb.MovingSum({int(slot.arg)}) = .{{}},\n"
                    )
                case "moving_max_tick":
                    out.append(
                        f"    {slot.name}: pb.MovingMax({int(slot.arg)}) = .{{}},\n"
                    )
                case "moving_min_tick":
                    out.append(
                        f"    {slot.name}: pb.MovingMin({int(slot.arg)}) = .{{}},\n"
                    )
                case "rising_edge":
                    out.append(f"    {slot.name}: pb.RisingEdge = .{{}},\n")
                case "falling_edge":
                    out.append(f"    {slot.name}: pb.FallingEdge = .{{}},\n")
                case "throttle_ms":
                    out.append(
                        f"    {slot.name}: pb.Throttle = .{{ .interval_us = {int(slot.arg)} }},\n"
                    )
                case "bar_tick":
                    out.append(
                        f"    {slot.name}: pb.Bar = .{{ .cap = {int(slot.arg)} }},\n"
                    )
                case "bar_time":
                    out.append(
                        f"    {slot.name}: pb.Bar = .{{ .window_ms = {int(slot.arg)} }},\n"
                    )
                case "count":
                    out.append(f"    {slot.name}: pb.Count = .{{}},\n")
                case "sample" | "debounce":
                    out.append(
                        f"    {slot.name}: pb.ClockEmitter = .{{ .period_ms = {int(slot.arg)} }},\n"
                    )

        out.append("};\n" "\n" "var state: State = .{};\n" "\n")

        # Group rules by filter, preserving first-seen order.
        seen: list[str] = []
        for r in self.rules:
            if r.filter not in seen:
                seen.append(r.filter)
        # `depth` and `payload_raw` are always passed in so the dispatcher's
        # call sites stay uniform. Discard them up front when this filter's
        # rules don't use them — Zig rejects unused params otherwise.
        for filt in seen:
            fn_name = _filter_to_fn_name(filt)
            filt_reentrant = any(r.reentrant for r in self.rules if r.filter == filt)
            filt_uses_raw = any(
                "payload_raw" in r.body_zig for r in self.rules if r.filter == filt
            )
            out.append(
                f"fn {fn_name}(payload_float: f64, payload_raw: []const u8, depth: u8) void {{\n"
            )
            if not filt_reentrant:
                out.append("    _ = depth;\n")
            if not filt_uses_raw:
                out.append("    _ = payload_raw;\n")
            for r in self.rules:
                if r.filter == filt:
                    out.append(r.body_zig)
            out.append("}\n\n")

        for req in self.reqs:
            fn_name = _request_to_fn_name(req.subject)
            uses_raw = "payload_raw" in req.body_zig
            uses_reply = "reply_subject" in req.body_zig
            out.append(
                f"fn {fn_name}(payload_float: f64, payload_raw: []const u8, reply_subject: []const u8) void {{\n"
            )
            if "payload_float" not in req.body_zig:
                out.append("    _ = payload_float;\n")
            if not uses_raw:
                out.append("    _ = payload_raw;\n")
            if not uses_reply:
                out.append("    _ = reply_subject;\n")
            out.append(req.body_zig)
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
            "    dispatchInternal(subject, payload, 0);\n"
            "}\n"
            "\n"
            "fn dispatchInternal(subject: []const u8, payload: []const u8, depth: u8) void {\n"
            "    if (depth >= pb.MAX_DEPTH) return;\n"
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
            "    // Top-level inputs are wired to NATS; re-entered subjects are\n"
            "    // already on the wire via pb.emitReentrant before we recurse.\n"
            "    if (depth == 0) pb.emit(subj_z, payload);\n"
            "    const v: f64 = strtod(pl_z, null);\n"
            "\n"
        )

        for i, filt in enumerate(seen):
            fn_name = _filter_to_fn_name(filt)
            kw = "if" if i == 0 else "} else if"
            out.append(
                f'    {kw} (eql(subject, "{filt}")) {{\n        {fn_name}(v, payload, depth);\n'
            )
        out.append("    }\n}\n")

        out.append(self._render_requests())


        if self.pumps:
            out.append(self._render_collect())
        out.append(self._render_clocks())
        return "".join(out)

    def _render_requests(self) -> str:
        out: list[str] = []
        out.append(
            "\n"
            "export fn tinyblok_nats_handle_msg(\n"
            "    subject_ptr: [*]const u8,\n"
            "    subject_len: usize,\n"
            "    reply_ptr: [*]const u8,\n"
            "    reply_len: usize,\n"
            "    payload_ptr: [*]const u8,\n"
            "    payload_len: usize,\n"
            ") callconv(.c) void {\n"
            "    if (payload_len + 1 > 64) return;\n"
            "    const subject = subject_ptr[0..subject_len];\n"
            "    const reply_subject = reply_ptr[0..reply_len];\n"
            "    const payload = payload_ptr[0..payload_len];\n"
            "\n"
            "    var pl_buf: [64]u8 = undefined;\n"
            "    @memcpy(pl_buf[0..payload.len], payload);\n"
            "    pl_buf[payload.len] = 0;\n"
            "    const pl_z: [*:0]const u8 = @ptrCast(&pl_buf);\n"
            "    const v: f64 = strtod(pl_z, null);\n"
            "\n"
        )
        for i, req in enumerate(self.reqs):
            fn_name = _request_to_fn_name(req.subject)
            kw = "if" if i == 0 else "} else if"
            out.append(
                f'    {kw} (eql(subject, "{req.subject}")) {{\n        {fn_name}(v, payload, reply_subject);\n'
            )
        if self.reqs:
            out.append("    }\n")
        out.append("}\n\n")

        out.append(
            "pub const RequestSub = extern struct {\n"
            "    subject: [*:0]const u8,\n"
            "};\n\n"
        )
        out.append(f"export const tinyblok_request_sub_count: usize = {len(self.reqs)};\n")
        if self.reqs:
            out.append(f"export const tinyblok_request_subs: [{len(self.reqs)}]RequestSub = .{{\n")
            for req in self.reqs:
                out.append(f'    .{{ .subject = "{req.subject}" }},\n')
            out.append("};\n")
        else:
            out.append("export const tinyblok_request_subs: [0]RequestSub = .{};\n")
        return "".join(out)

    def _render_clocks(self) -> str:
        """Generate per-clock-slot fire functions plus the C-visible
        clock-slot table. Each fire fn runs the slot's deadline action,
        emits if due, and re-arms the slot when it remains active. The C
        side (drivers.c) creates one
        `esp_timer_handle_t` per entry in `tinyblok_clock_slots[]` and
        dispatches its callback into the matching `fire`."""
        clock_slots = [s for s in self.slots if s.kind in ("bar_time", "sample", "debounce")]
        out: list[str] = []
        # Always emit the helper + extern + table so the C side has a stable
        # symbol surface, even when there are zero clock slots.
        out.append(
            "\nextern fn tinyblok_clock_arm(slot_id: usize, us_until: u64) void;\n"
            "\n"
            "fn deadlineUsFromNow(deadline_ms: i64) u64 {\n"
            "    const now_ms = tinyblok_now_ms();\n"
            "    const diff = deadline_ms - now_ms;\n"
            "    if (diff <= 0) return 0;\n"
            "    return @as(u64, @intCast(diff)) * 1000;\n"
            "}\n"
        )
        for slot in clock_slots:
            fn_name = f"tinyblok_clock_{slot.name}_fire"
            out.append(f"\nexport fn {fn_name}() callconv(.c) void {{\n")
            out.append(f"    const now_ms: i64 = tinyblok_now_ms();\n")
            if slot.kind == "bar_time":
                out.append(
                    f"    if (state.{slot.name}.timeTick(now_ms)) |__c| {{\n"
                )
                for f in ("open", "high", "low", "close"):
                    out.append(
                        f'        pb.emitFloat("{slot.subject}.bar.{f}", __c.{f}, 6);\n'
                    )
                out.append("    }\n")
            elif slot.kind == "sample":
                out.append(
                    f"    if (state.{slot.name}.fireSample(now_ms)) {{\n"
                    f'        pb.emit("{slot.subject}", state.{slot.name}.payloadSlice());\n'
                    f"    }}\n"
                )
            elif slot.kind == "debounce":
                out.append(
                    f"    if (state.{slot.name}.fireDebounce(now_ms)) {{\n"
                    f'        pb.emit("{slot.subject}", state.{slot.name}.payloadSlice());\n'
                    f"    }}\n"
                )
            slot_id = self._clock_slot_id(slot.name)
            out.append(
                f"    if (state.{slot.name}.nextDeadlineMs()) |__dl| {{\n"
                f"        tinyblok_clock_arm({slot_id}, deadlineUsFromNow(__dl));\n"
                f"    }}\n"
                f"}}\n"
            )
        out.append(
            "\npub const ClockSlot = extern struct {\n"
            "    fire: *const fn () callconv(.c) void,\n"
            "};\n"
        )
        out.append(f"\nexport const tinyblok_clock_slot_count: usize = {len(clock_slots)};\n")
        if clock_slots:
            out.append(f"export const tinyblok_clock_slots: [{len(clock_slots)}]ClockSlot = .{{\n")
            for slot in clock_slots:
                out.append(f"    .{{ .fire = &tinyblok_clock_{slot.name}_fire }},\n")
            out.append("};\n")
        else:
            # Empty array: keep the symbol so drivers.c can link unconditionally.
            out.append("export const tinyblok_clock_slots: [0]ClockSlot = .{};\n")
        return "".join(out)

    def _render_collect(self) -> str:
        out: list[str] = []
        out.append(
            "\n"
            "fn dispatchFmt(subject: []const u8, comptime fmt: [*:0]const u8, args: anytype) void {\n"
            "    var buf: [32]u8 = undefined;\n"
            "    const n = @call(.auto, snprintf, .{ &buf, buf.len, fmt } ++ args);\n"
            "    if (n <= 0) return;\n"
            "    const len: usize = @min(@as(usize, @intCast(n)), buf.len - 1);\n"
            "    dispatch(subject, buf[0..len]);\n"
            "}\n"
            "\n"
        )
        # One exported per-pump function the C driver layer invokes from the
        # esp_event handler. Each pump owns its own esp_timer/event_id; the
        # tx_ring is drained separately by the main loop, so no flush here.
        for p in self.pumps:
            entry = _TYPE_TABLE[p.type_]
            pre = entry["pre"].replace("{x}", f"{p.from_sym}()")
            fmt = entry["fmt"]
            fn = _pump_fn_name(p.subject)
            out.append(
                f"export fn {fn}() callconv(.c) void {{\n"
                f'    dispatchFmt("{p.subject}", "{fmt}", .{{{pre}}});\n'
                f"}}\n\n"
            )
        # Pump table consumed by drivers.c. Period in microseconds (esp_timer's unit).
        out.append("pub const Pump = extern struct {\n")
        out.append("    subject: [*:0]const u8,\n")
        out.append("    period_us: u64,\n")
        out.append("    fire: *const fn () callconv(.c) void,\n")
        out.append("};\n\n")
        out.append(f"export const tinyblok_pump_count: usize = {len(self.pumps)};\n")
        out.append(f"export const tinyblok_pumps: [{len(self.pumps)}]Pump = .{{\n")
        for p in self.pumps:
            period_us = 1_000_000 // p.hz
            fn = _pump_fn_name(p.subject)
            out.append(
                f'    .{{ .subject = "{p.subject}", .period_us = {period_us}, .fire = &{fn} }},\n'
            )
        out.append("};\n")
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
        if n == "uptime-us":
            return "@as(f64, @floatFromInt(tinyblok_uptime_us()))"
        if n == "uptime-s":
            return "@as(f64, @floatFromInt(tinyblok_uptime_us())) / 1_000_000.0"
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


def _pump_fn_name(subject: str) -> str:
    prefix = "tinyblok."
    tail = subject[len(prefix):] if subject.startswith(prefix) else subject
    out = ["tinyblok_pump_"]
    for c in tail:
        out.append("_" if c in ".-" else c)
    return "".join(out)


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


def _request_to_fn_name(subject: str) -> str:
    prefix = "tinyblok."
    tail = subject[len(prefix) :] if subject.startswith(prefix) else subject
    req_prefix = "req."
    if tail.startswith(req_prefix):
        tail = tail[len(req_prefix):]
    out = ["req"]
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
        elif head == "on-req":
            em.emit_request(form)
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
