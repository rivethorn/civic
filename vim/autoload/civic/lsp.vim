" autoload/civic/lsp.vim — LSP server management for Civic
" Part of the Civic vim/neovim plugin
"
" Provides Start / Stop / Restart for the Civic LSP server (`civic lsp`).
" The LSP server speaks JSON-RPC over stdio and provides:
"   - Text document synchronization (full sync)
"   - Completion (keywords, types, built-ins, user-defined functions)
"   - Hover (function signatures, type descriptions)
"   - Go-to-definition (across files)
"   - Publish diagnostics (lexical, parse, semantic errors)
"
" Usage:
"   :CivicLspStart    — start the LSP server (auto-detected client)
"   :CivicLspStop     — stop the LSP server
"   :CivicLspRestart  — restart the LSP server

" ---------------------------------------------------------------------------
" s:RootDirForBuffer() — Find the project root for the current buffer
" ---------------------------------------------------------------------------
function! s:RootDirForBuffer() abort
  let root = civic#ProjectRoot()
  if !empty(root)
    return root
  endif
  return fnamemodify(expand('%:p:h'), ':p:h')
endfunction

" ---------------------------------------------------------------------------
" s:RegisterVimLsp() — Register the Civic server with vim-lsp
" Requires vim-lsp (https://github.com/prabirsharma/vim-lsp)
" ---------------------------------------------------------------------------
function! s:RegisterVimLsp() abort
  if !exists('#lsp')
    return 0
  endif
  let existing = lsp#registeredServers()
  for srv in existing
    if srv.name ==# 'civiclsp'
      return 0
    endif
  endfor

  call lsp#registerServer({
        \ 'name': 'civiclsp',
        \ 'cmd': {-> [civic#Executable(), 'lsp']},
        \ 'allowlist': ['civic'],
        \ 'root_uri': {-> s:RootDirForBuffer()},
        \ 'initializationOptions': {},
        \ })
  return 1
endfunction

" ---------------------------------------------------------------------------
" civic#lsp#Start() — Start the Civic LSP server
" ---------------------------------------------------------------------------
function! civic#lsp#Start() abort
  if !civic#HasExecutable()
    echohl ErrorMsg | echo "civic: executable not found in PATH" | echohl None
    return
  endif

  " Neovim — prefer our Lua module for lspconfig setup
  if has('nvim')
    lua << EOF
local ok, civic_lua = pcall(require, 'civic')
if ok and civic_lua.setup then
  civic_lua.setup()
else
  -- No Lua module or setup failed; fall back to manual start
  local exec = 'civic'
  local root = vim.fn.civic#ProjectRoot()
  if root == '' then
    local path = vim.api.nvim_buf_get_name(0)
    root = vim.fn.fnamemodify(path, ':p:h')
  end
  vim.lsp.start({
    name = "civic",
    cmd = {exec, "lsp"},
    filetypes = {"civic"},
    root_dir = root,
  })
end
EOF
    echohl MoreMsg | echo "Civic LSP started" | echohl None
    return
  endif

  " Vim with vim-lsp
  if exists('g:lsp') || exists('#lsp')
    call s:RegisterVimLsp()
    execute 'LspStart civiclsp'
    echohl MoreMsg | echo "Civic LSP started (vim-lsp)" | echohl None
    return
  endif

  echohl WarningMsg | echo "civic lsp: no LSP client detected." | echohl None
  echo "  Install nvim-lspconfig (Neovim) or vim-lsp (Vim) for LSP support."
  echo "  Syntax highlighting and CLI commands work without an LSP client."
endfunction

" ---------------------------------------------------------------------------
" civic#lsp#Stop() — Stop the Civic LSP server
" ---------------------------------------------------------------------------
function! civic#lsp#Stop() abort
  if has('nvim')
    " Neovim: stop the civic client
    lua << EOF
local clients = vim.lsp.get_clients({name = "civic"})
for _, client in ipairs(clients) do
  client:stop()
end
if #clients == 0 then
  print("Civic LSP: no active client found")
else
  print("Civic LSP: stopped " .. #clients .. " client(s)")
end
EOF
    return
  endif

  " Vim with vim-lsp
  if exists('g:lsp') || exists('#lsp')
    execute 'LspStop civiclsp'
    echohl MoreMsg | echo "Civic LSP stopped" | echohl None
    return
  endif

  echohl WarningMsg | echo "civic lsp: no LSP client is active." | echohl None
endfunction

" ---------------------------------------------------------------------------
" civic#lsp#Restart() — Restart the Civic LSP server
" ---------------------------------------------------------------------------
function! civic#lsp#Restart() abort
  call civic#lsp#Stop()
  " Brief pause to let the server shut down cleanly
  call timer_start(500, {-> civic#lsp#Start()})
endfunction

