" ftplugin/civic.vim — Civic filetype settings
" Part of the Civic vim/neovim plugin

" ---------------------------------------------------------------------------
" Comments
" ---------------------------------------------------------------------------
setlocal commentstring=//\ %s
setlocal comments=://,:///,://!,b:/*,*:*,b:*/,://

" ---------------------------------------------------------------------------
" File types / extensions
" ---------------------------------------------------------------------------
setlocal suffixes=.cv

" ---------------------------------------------------------------------------
" Build / compile integration
" ---------------------------------------------------------------------------
" :make  will run  civic build  (project mode or file mode)
setlocal include=\import
setlocal suffixesadd=.cv

" Error format for diagnostics produced by the Civic compiler:
"   path/to/file.cv:10:3: error: message
"   path/to/file.cv:10:3: warning: message
"   path/to/file.cv:10:3: note: message
setlocal errorformat=%f:%l:%c:\ error:\ %m
setlocal errorformat+=%f:%l:%c:\ warning:\ %m
setlocal errorformat+=%f:%l:%c:\ note:\ %m

" ---------------------------------------------------------------------------
" iskeyword — treat identifiers normally
" (allows  std.io  to be parsed as  std  +  .  +  io  for word motions)
" ---------------------------------------------------------------------------
setlocal iskeyword=@,#,$,_,1-9,-,@

" ---------------------------------------------------------------------------
" Match pairs — ( ) { } [ ] only
" (< > is excluded because Civic uses angle brackets for templates/comparisons)
" ---------------------------------------------------------------------------
setlocal matchpairs=(:),{:},[:]

" ---------------------------------------------------------------------------
" Indentation — handled by indent/civic.vim (cindent)
" ---------------------------------------------------------------------------
" (no settings needed here; vim sources indent/civic.vim automatically)

" ---------------------------------------------------------------------------
" Auto-indentation
" ---------------------------------------------------------------------------
setlocal autoindent
setlocal smartindent

" ---------------------------------------------------------------------------
" Tab settings — Civic convention is 4-space tabs
" ---------------------------------------------------------------------------
setlocal noexpandtab
setlocal tabstop=4
setlocal shiftwidth=4
setlocal softtabstop=4

" ---------------------------------------------------------------------------
" Folding — syntax-based so braces create fold regions
" ---------------------------------------------------------------------------
setlocal foldmethod=syntax
setlocal foldtext=

" ---------------------------------------------------------------------------
" Tagbar support (if installed)
" ---------------------------------------------------------------------------
" Tagbar can parse Civic for tags using Universal Ctags.
" No special configuration needed — tagbar will pick up .cv files
" automatically once ctags supports the civic filetype.

" vim: set ft=vim et sw=2 sts=2:

