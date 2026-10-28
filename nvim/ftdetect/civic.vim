" ftdetect/civic.vim — Civic filetype detection
" Part of the Civic vim/neovim plugin
"
" Sets filetype=civic for all .cv source files so that the syntax,
" indent, and ftplugin files are loaded automatically.

au BufNewFile,BufRead *.cv  setfiletype civic
