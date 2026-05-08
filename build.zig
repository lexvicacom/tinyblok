const std = @import("std");

pub fn build(b: *std.Build) void {
    const target = b.standardTargetOptions(.{});
    const optimize = b.standardOptimizeOption(.{ .preferred_optimize_mode = .Debug });

    const manifest_mod = b.createModule(.{
        .root_source_file = b.path("build.zig.zon"),
    });

    const soundcheck_mod = b.createModule(.{
        .root_source_file = b.path("main/zig/soundcheck.zig"),
        .target = target,
        .optimize = optimize,
        .link_libc = true,
        .imports = &.{
            .{ .name = "manifest", .module = manifest_mod },
        },
    });

    const soundcheck_exe = b.addExecutable(.{
        .name = "soundcheck",
        .root_module = soundcheck_mod,
    });

    const install_soundcheck = b.addInstallArtifact(soundcheck_exe, .{
        .dest_dir = .{ .override = .prefix },
        .dest_sub_path = "soundcheck",
    });

    const soundcheck_step = b.step("soundcheck", "Build the host patchbay debugger CLI");
    soundcheck_step.dependOn(&install_soundcheck.step);
}
