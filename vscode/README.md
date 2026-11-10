# Nest

VS Code syntax highlighting for `.nest` grammar files.

## Try it

1. Open this folder in VS Code.
2. Press `F5` to launch an Extension Development Host.
3. Open `specs/bootstrap.nest`.

## Install

```sh
pnpm run deploy
```

This packages the `.vsix` and installs it with `code --install-extension`.
The same script is wired to the default build task (`Cmd+Shift+B`).

## Package manually

```sh
pnpm install
pnpm dlx @vscode/vsce package --allow-missing-repository
```
