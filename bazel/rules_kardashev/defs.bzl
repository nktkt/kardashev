"""rules_kardashev — Starlark rules for building kardashev programs with Bazel.

Public surface:
    * `kardashev_library(name, srcs, deps)`  — groups `.kd` files into a unit
      that other libraries / binaries can depend on. No build action runs;
      kardc reads files at compile time and the rule is purely a propagator.
    * `kardashev_binary(name, src, deps)`    — AOT-compiles `src` (the entry
      `.kd` file) via `//compiler:kardc -o <name>` and exposes the result
      as an executable Bazel target.

Both rules use a custom `KardashevInfo` provider to carry the transitive
closure of `.kd` files. The compile action depends on every reachable
source so Bazel re-runs kardc whenever any module changes.

Module resolution still happens at kardc time (`mod foo;` reads
`foo.kd` next to the entry file). As long as the `srcs` of dependent
libraries live in the same Bazel package as the binary's `src`, Bazel's
sandbox places them in the same directory and resolution works.
"""

KardashevInfo = provider(
    doc = "Transitive `.kd` sources reachable from a kardashev target.",
    fields = {
        "sources": "depset of File — every .kd file reachable from this target",
    },
)

def _kardashev_library_impl(ctx):
    transitive = [dep[KardashevInfo].sources for dep in ctx.attr.deps]
    sources = depset(
        direct = ctx.files.srcs,
        transitive = transitive,
    )
    return [
        KardashevInfo(sources = sources),
        DefaultInfo(files = sources),
    ]

kardashev_library = rule(
    implementation = _kardashev_library_impl,
    doc = "Group of kardashev `.kd` source files reusable across targets.",
    attrs = {
        "srcs": attr.label_list(
            allow_files = [".kd"],
            doc = "Source `.kd` files exposed by this library.",
        ),
        "deps": attr.label_list(
            providers = [KardashevInfo],
            doc = "Other kardashev_library targets this library transitively pulls in.",
        ),
    },
)

def _kardashev_binary_impl(ctx):
    out = ctx.actions.declare_file(ctx.label.name)
    transitive = [dep[KardashevInfo].sources for dep in ctx.attr.deps]
    all_sources = depset(
        direct = [ctx.file.src],
        transitive = transitive,
    )

    args = ctx.actions.args()
    args.add("-o", out.path)
    args.add(ctx.file.src.path)

    ctx.actions.run(
        executable = ctx.executable._kardc,
        arguments = [args],
        inputs = all_sources,
        outputs = [out],
        mnemonic = "KardashevCompile",
        progress_message = "Compiling kardashev binary %{label}",
        # kardc shells out to clang for the link step. On Linux,
        # Bazel's default sandbox hides the system `ld` from clang
        # (only the explicitly-declared inputs are visible). Mark the
        # action no-sandbox so the host's linker stays reachable. The
        # trade-off — losing strict input declaration for this step —
        # is acceptable because the link only consumes the .o we just
        # produced + the system libc that kardc programs always need.
        execution_requirements = {"no-sandbox": "1"},
        # Forward the host environment (PATH etc) so clang finds its
        # toolchain components on both Linux and macOS.
        use_default_shell_env = True,
    )

    return [
        DefaultInfo(executable = out, files = depset([out])),
    ]

kardashev_binary = rule(
    implementation = _kardashev_binary_impl,
    executable = True,
    doc = "AOT-compile a kardashev `.kd` entry file into a native executable.",
    attrs = {
        "src": attr.label(
            allow_single_file = [".kd"],
            doc = "Entry `.kd` file. Its `main` function becomes the executable's entry point.",
        ),
        "deps": attr.label_list(
            providers = [KardashevInfo],
            doc = "kardashev_library targets whose `.kd` files this binary can `mod`-import.",
        ),
        "_kardc": attr.label(
            default = "//compiler:kardc",
            executable = True,
            cfg = "exec",
            doc = "The kardc compiler binary. Defaults to the in-tree build.",
        ),
    },
)
