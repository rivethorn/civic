# Civic — Neovim Plugin

Syntax highlighting, indentation, CLI integration, and LSP support for the **Civic** programming language (`.cv` files) in Neovim.

## Features

| Feature | Description |
|---|---|
| **Filetype detection** | `.cv` files are automatically set to `filetype=civic` |
| **Syntax highlighting** | Keywords, types, constants, built-in functions, `#include <std/io>` module directives, `#header` markers, comments, strings, numbers, operators, delimiters |
| **Smart indentation** | C-style `cindent` with Civic-specific `cinoptions` |
| **Filetype settings** | `commentstring`, `comments`, `errorformat`, `gf`/include-search on `#include <module>` |
| **CLI commands** | `:CivicBuild`, `:CivicRun`, `:CivicTest`, `:CivicEmitIR`, `:CivicCheck`, `:CivicNew`, `:CivicInfo` |
| **Quickfix integration** | Compiler diagnostics are parsed into the quickfix list with real `file:line:col` entries — navigable with `:cnext`/`:cprev` |
| **Terminal integration** | `:CivicRun` opens a terminal split for live program output |
| **LSP support** | Automatic attachment via Neovim's native `vim.lsp` (0.11+); no lspconfig dependency |
| **Health check** | `:checkhealth civic` verifies the executable, root, stdlib, and LSP state |

All external commands run asynchronously through `vim.system()` with argument
lists — no shell strings, so command lines cannot be broken by quoting or by
your `$SHELL` (fish, zsh, …).

## Installation

### Using a plugin manager

**lazy.nvim** (the plugin lives in the `nvim/` subdirectory):

```lua
return {
  "rivethorn/civic",
  lazy = true,
  ft = "civic",
  config = function(plugin)
    vim.opt.rtp:append(plugin.dir .. "/nvim")
    require("civic").setup()
  end,
}
```

**packer.nvim:**

```lua
use { 'rivethorn/civic' }
```

### Manual installation

The plugin root is the `nvim/` directory of this repository:

```sh
git clone https://github.com/rivethorn/civic.git
cd civic
mkdir -p ~/.local/share/nvim/site/pack/civic/start
ln -s "$(pwd)/nvim" ~/.local/share/nvim/site/pack/civic/start/civic
```

### Neovim setup (Lua config)

The plugin self-installs when loaded; explicit setup is only needed for options:

```lua
require('civic').setup()
```

Or with custom options:

```lua
require('civic').setup({
  executable = '/usr/local/bin/civic',
  on_attach = function(client, bufnr) ... end,
})
```

## Commands

| Command | Description |
|---|---|
| `:CivicBuild [args]` | Build the current project (`civic.toml`) or current `.cv` file. Diagnostics appear in the quickfix list. |
| `:CivicRun [args]` | Build and run in a terminal split. Extra args before `--` are passed to `civic run` (e.g. `--release`); anything after `--` goes to the program. |
| `:CivicTest [paths]` | Run the test suite (`civic test`) and show results with PASS/FAIL highlighting. |
| `:CivicEmitIR` | Emit LLVM IR (`civic emit-ir`) and display it in a scratch buffer with `llvm` highlighting. |
| `:CivicCheck` | Quick syntax/semantic check via `emit-ir` — errors go to quickfix, no binary produced. |
| `:CivicNew [name]` | Create a new Civic project (`civic new <name>`). Prompts for a name if omitted. |
| `:CivicInfo` | Show plugin, executable, and project information. |
| `:CivicLspStart` | Start the Civic LSP server (native `vim.lsp`). |
| `:CivicLspStop` | Stop the Civic LSP server. |
| `:CivicLspRestart` | Restart the Civic LSP server. |

## Default Key Mappings

All mappings are buffer-local on `civic` filetypes and prefixed with `<leader>c`:

| Mapping | Target | Description |
|---|---|---|
| `<leader>cb` | `:CivicBuild` | Build |
| `<leader>cr` | `:CivicRun` | Run |
| `<leader>ct` | `:CivicTest` | Test |
| `<leader>cc` | `:CivicCheck` | Quick check |
| `<leader>ci` | `:CivicEmitIR` | Emit IR |
| `<leader>cl` | `:CivicLspRestart` | Restart LSP |
| `<leader>cI` | `:CivicInfo` | Plugin info |

Disable all mappings with:

```lua
require('civic').setup({ keymaps = false })
-- or globally, before the plugin loads:
vim.g.civic_no_mappings = 1
```

## Configuration

Set these in your `init.lua` **before** loading the plugin:

| Variable | Default | Description |
|---|---|---|
| `vim.g.civic_executable` | auto-detect | Override the path to the `civic` binary |
| `vim.g.civic_no_mappings` | 0 | Set to 1 to disable default key mappings |
| `vim.g.civic_auto_build` | 0 | Set to 1 to run `:CivicCheck` on every save of a `.cv` file |

Or pass options to `require('civic').setup({ ... })`:

| Option | Default | Description |
|---|---|---|
| `executable` | auto-detect | Path to the `civic` binary |
| `auto_lsp` | `true` | Attach `civic lsp` to `.cv` buffers |
| `auto_check` | `false` | Run `:CivicCheck` after saving a `.cv` file |
| `keymaps` | `true` | Create the `<leader>c*` buffer-local mappings |
| `open_quickfix` | `'auto'` | Open the quickfix window on errors (`'always'`/`'never'`) |
| `on_attach` | `nil` | Callback invoked with `(client, bufnr)` on every LSP attach |

## Project Detection

The plugin searches for `civic.toml` in the current file's ancestor
directories (`vim.fs.root`). When found, all CLI commands operate in
**project mode** using `--manifest-path`; when none is found, they fall back to
**file mode** (operating on the current `.cv` file).

`gf` on `#include <std/io>` resolves module name → header: the plugin looks in
the stdlib of the discovered Civic root (`CIVIC_ROOT`, or resolved next to the
executable), then in the including file's directory.

## Civic CLI Reference

```
civic new <name>                     Create a new project
civic build [--release]              Build the current project
civic run [--release] [-- <args>]    Build and run
civic test [file.cv|dir ...]         Run tests
civic emit-ir <source.cv>            Emit LLVM IR
civic lsp                            Start LSP server (stdio)
```

## License

See the project root for license information.
