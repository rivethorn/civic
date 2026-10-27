# Civic — Vim/Neovim Plugin

Syntax highlighting, indentation, CLI integration, and LSP support for the
**Civic** programming language (`.cv` files) in Vim and Neovim.

## Features

| Feature | Description |
|---|---|
| **Filetype detection** | `.cv` files are automatically set to `filetype=civic` |
| **Syntax highlighting** | Keywords, types, constants, built-in functions, standard library modules, comments, strings, numbers, operators, delimiters, and import paths |
| **Smart indentation** | C-style `cindent` with Civic-specific `cinoptions` |
| **Filetype settings** | `commentstring`, `errorformat`, `makeprg`-ready, `iskeyword`, `matchpairs`, folding |
| **CLI commands** | `:CivicBuild`, `:CivicRun`, `:CivicTest`, `:CivicEmitIR`, `:CivicCheck`, `:CivicNew`, `:CivicInfo` |
| **Quickfix integration** | Compiler diagnostics are automatically parsed into the quickfix window |
| **Terminal integration** | `:CivicRun` opens a terminal split for live program output |
| **LSP support** | Full LSP client integration via `nvim-lspconfig` (Neovim) or `vim-lsp` (Vim) |

## Installation

### Using a plugin manager

**vim-plug:**
```vim
Plug 'rivethorn/civic', { 'rtp': 'vim' }
```

**packer.nvim:**
```lua
use { 'rivethorn/civic', rtp = 'vim' }
```

**lazy.nvim:**
```lua
{ 'rivethorn/civic', lazy = true, rtp = 'vim', ft = 'civic' }
```

### Manual installation

```sh
git clone https://github.com/rivethorn/civic.git
cd civic
mkdir -p ~/.vim/pack/civic/start
ln -s "$(pwd)/vim" ~/.vim/pack/civic/start/civic
```

### Neovim setup (Lua config)

After installing the plugin, add to your `init.lua`:

```lua
-- Start the Civic LSP automatically for .cv files
require('civic').setup()
```

Or start it manually:

```vim
:CivicLspStart
```

### Vim setup (vim-lsp)

If using [vim-lsp](https://github.com/prabirsharma/vim-lsp):

```vim
"CivicLspStart will auto-register and start the server
```

No extra configuration is needed — the plugin auto-detects vim-lsp.

## Commands

| Command | Description |
|---|---|
| `:CivicBuild` | Build the current project (`civic.toml`) or current `.cv` file. Diagnostics appear in the quickfix window. |
| `:CivicRun` | Build and run in a terminal split. Project mode uses `civic run`; file mode uses `civic run <file>`. |
| `:CivicTest` | Run the test suite (`civic test`) and show results. |
| `:CivicEmitIR` | Emit LLVM IR (`civic emit-ir`) and display in a scratch buffer with `llvm` syntax. |
| `:CivicCheck` | Quick syntax/semantic check via `emit-ir` — errors go to quickfix, no binary produced. |
| `:CivicNew` | Create a new Civic project (`civic new <name>`). Prompts for a name if none provided. |
| `:CivicInfo` | Show plugin, executable, and project information. |
| `:CivicLspStart` | Start the Civic LSP server for the current file. |
| `:CivicLspStop` | Stop the Civic LSP server. |
| `:CivicLspRestart` | Restart the Civic LSP server. |

## Default Key Mappings

All mappings are prefixed with `<leader>c`:

| Mapping | Command | Description |
|---|---|---|
| `<leader>cb` | `:CivicBuild` | Build |
| `<leader>cr` | `:CivicRun` | Run |
| `<leader>ct` | `:CivicTest` | Test |
| `<leader>cc` | `:CivicCheck` | Quick check |
| `<leader>ci` | `:CivicEmitIR` | Emit IR |
| `<leader>cl` | `:CivicLspStart` | Start LSP |
| `<leader>cI` | `:CivicInfo` | Plugin info |

Disable all mappings with:
```vim
let g:civic_no_mappings = 1
```

## Configuration

Set these in your `init.vim` or `init.lua` **before** loading the plugin:

| Variable | Default | Description |
|---|---|---|
| `g:civic_executable` | auto-detect | Override the path to the `civic` binary |
| `g:civic_no_mappings` | 0 | Set to 1 to disable default key mappings |
| `g:civic_auto_build` | 0 | Set to 1 to run `:CivicCheck` on every save of a `.cv` file |

## Language Features Highlighted

### Keywords
```
if  else  while  for  return  import  class  struct  public  private  template  typename  move
```

### Types
```
void  bool  char  int  float  double  String  vector  unique_ptr  shared_ptr  const
```

### Constants
```
true  false
```

### Built-in Functions
```
print  println  assert  move
```

### Standard Library Modules
```
std.io  std.core  std.math  std.numeric  std.string  std.algorithm
```

### Operators
```
+  -  *  /  %  ==  !=  <  <=  >  >=  &&  ||  !  =  +=  -=  *=  /=  ++  --  .
```

## Project Detection

The plugin automatically searches for `civic.toml` by walking up from the
current file's directory. When found, all CLI commands operate in
**project mode**. When no `civic.toml` is found, they fall back to
**file mode** (operating on the current `.cv` file).

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
