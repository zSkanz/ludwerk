# Contributing to Ludwerk

Thank you for your interest in Ludwerk. This page explains how to set up, what
the project expects from a change, and how changes are reviewed.

## Before you start

- For a bug, open an issue with the steps to reproduce it, what you expected
  and what happened. Attach the engine log if you have one.
- For a feature or a change in behavior, open an issue first so the design can
  be agreed before the code is written.
- Small fixes to documentation, examples and tooling can go straight to a pull
  request.

## Setting up

Run `scripts/bootstrap.ps1` on Windows or `./scripts/bootstrap.sh` on Linux and
macOS. The script checks your toolchain and installs the pinned tools.

Build and test with the CMake presets:

```sh
cmake --preset win-msvc-dev
cmake --build --preset win-msvc-dev
ctest --preset win-msvc-dev
```

Before you open a pull request, run the local checks:

```sh
scripts/localgate.ps1
```

They build and test on Windows, build and test on Linux in a container, and run
the documentation, formatting and Luau checks. The same checks run in
continuous integration. The Linux part needs Docker; without it, pass
`-SkipLinux`, and continuous integration builds Linux for you.

## Engine rules

The code and the documents cite these by number.

- **R1. English everywhere**: code, comments, identifiers, commit messages and
  documentation.
- **R2. All Luau is `--!strict`**, under the new type solver.
- **R3. No hard-coded user-facing text.** Every message goes through the
  translation catalog, in the engine, the editor and the tools.
- **R4. Scripts are always sandboxed.** No build and no option turns the
  sandbox off.
- **R5. Pinned versions only.** Every dependency and every tool is named at an
  exact version, and a change of one needs a design decision record.
- **R6. Permissive licenses only**, for anything the engine links or ships.
- **R7. Clean room.** Ludwerk reproduces public API concepts only. Do not
  contribute code, assets or branding from another platform, or anything
  derived from decompiled material.
- **R8. Signals are deferred.** A handler runs after the code that fired the
  signal, never inside it.
- **R9. A vector is three 32-bit floats** in a script, and the engine keeps
  world coordinates in 64 bits.
- **R10. The simulation is deterministic**: no wall-clock time, no unseeded
  randomness and no unordered iteration in anything the simulation reads, and
  parallel work commits in a fixed order.
- **R11. The main branch only moves to a commit that passed every check.**
- **R12. Conventional commit messages.**
- **R13. Never edit `third_party/` in place.** Put a patch under
  `third_party/patches/` and list it in the manifest.
- **R14. Builds are out of tree.** Nothing is built inside the repository.
- **R15. New platforms and large features are opened by the maintainer.**
  Open an issue before building one.
- **R16. Performance is judged with the interpreter**: nothing depends on a
  just-in-time compiler, which some platforms do not allow.
- **R17. No graphics, physics or audio backend type appears in the public
  API.**
- **R18. Rendering is judged against a stated reference**, not by eye.

Style: C++ follows `.clang-format`; Luau follows `stylua.toml`. Comments
explain why, not what.

## Commits and pull requests

- Contributions are accepted under the
  [Developer Certificate of Origin](https://developercertificate.org/). Sign
  off each commit with `git commit -s`.
- Use conventional commit messages (`feat:`, `fix:`, `docs:`, `test:`,
  `chore:`), with the module as the scope where it helps, for example
  `fix(scene): ...`.
- A pull request needs green continuous integration.
- A change to the API comes with matching conformance tests and regenerated
  API files.
- A change in behavior or design comes with an updated or new record in
  [`docs/decisions/`](docs/decisions/) and the matching documentation in the
  same pull request.
- A bug fix comes with a test that fails without the fix.

## License

Ludwerk is licensed under [Apache-2.0](LICENSE). By contributing, you agree
that your contribution is licensed under the same terms.
