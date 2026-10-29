-- ftplugin/civic.lua — buffer-local options for Civic sources.
-- This replaces the old FileType autocmd, which aborted halfway through on
-- an invalid 'comments' value and left every setting after it unapplied.

local bufnr = vim.api.nvim_get_current_buf()
if vim.b[bufnr].did_civic_ftplugin then
  return
end
vim.b[bufnr].did_civic_ftplugin = 1

-- Commenting (valid 'comments' entries: flags then ':' then the string).
vim.bo[bufnr].commentstring = '// %s'
vim.bo[bufnr].comments = 's1:/*,mb:*,ex:*/,://'

-- 'gf' / include search on #include <std/io>.
vim.bo[bufnr].suffixesadd = '.cv'
vim.bo[bufnr].include = [[#include\s*<\zs[^>]*\ze>]]
vim.bo[bufnr].includeexpr = 'v:lua.require("civic.util").module_to_path(v:fname)'

-- Match compiler output (:cfile / :CivicCheck quickfix entries).
vim.bo[bufnr].errorformat = [[%f:%l:%c: error: %m]]

-- C-style editing.
vim.bo[bufnr].cindent = true
vim.bo[bufnr].cinoptions = '>s,e-s,n-s'
vim.bo[bufnr].tabstop = 4
vim.bo[bufnr].shiftwidth = 4
vim.bo[bufnr].softtabstop = 4
vim.bo[bufnr].expandtab = false

vim.bo[bufnr].iskeyword = '@,48-57,_,192-255,$'

-- Undo local settings when the filetype changes away from civic.
-- NOTE: one :setlocal consumes the whole line — never concatenate a second
-- 'setlocal' into the same command (it parses as an option name -> E518).
vim.b[bufnr].undo_ftplugin = table.concat({
  'setlocal commentstring< comments< suffixesadd< include< includeexpr<',
  'errorformat< cindent< cinoptions< tabstop< shiftwidth<',
  'softtabstop< expandtab< iskeyword<',
  '| unlet! b:did_civic_ftplugin',
}, ' ')

require('civic').buffer_setup(bufnr)
