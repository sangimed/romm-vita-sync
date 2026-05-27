# romm-vita-sync

<p align="center">
    <img src="assets/logo.png" alt="RoMM Vita Sync logo" width="420">
</p>

Save synchronization client for RomM on PlayStation Vita, supporting native and emulated platforms.

## Project Status

**Stage:** early development.

This project is under active development.

## About

romm-vita-sync is a PlayStation Vita homebrew project focused on save synchronization with RomM.

The goal is to provide a simple and safe save sync experience on Vita.

## Documentation

All detailed information is available in the project documentation:

- [Stable Documentation](https://sangimed.github.io/romm-vita-sync/stable/) from `master`
- [Nightly Documentation](https://sangimed.github.io/romm-vita-sync/nightly/) from `develop`
- [Docs Source](docs/index.md)

The documentation workflow publishes versioned MkDocs output to the `gh-pages` branch with `mike`; GitHub Pages must be configured to serve the `gh-pages` branch root. If `nightly` is deployed before `stable`, it temporarily becomes the default channel.

## Contributing

Issues and pull requests are welcome.

## Acknowledgements

Special thanks to the projects and maintainers this work builds on:

- [`vita-mcr2vmp`](https://github.com/dots-tb/vita-mcr2vmp) by `@dots_tb`, included as a submodule under `tools/vita-mcr2vmp` and used for Vita/PS1 memory card conversion support.
- [`vitasdk-docker`](https://github.com/gnuton/vitasdk-docker) by `@gnuton`, whose Docker image work was used as the basis for this repository's `Dockerfile`.
