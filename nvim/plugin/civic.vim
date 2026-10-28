" plugin/civic.vim — Civic Neovim plugin (Lua core)
if exists('g:loaded_civic') | finish | endif
let g:loaded_civic = 1
lua require('civic').setup()
