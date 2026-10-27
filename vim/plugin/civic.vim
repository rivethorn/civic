" plugin/civic.vim — Civic vim/neovim plugin entry point
"
" Defines user commands and optional key mappings for the Civic
" language toolchain.  The actual logic lives in autoload/civic.vim
" and autoload/civic/lsp.vim (loaded on demand).

if exists("g:loaded_civic")
  finish
endif
let g:loaded_civic = 1

" ---------------------------------------------------------------------------
" User configuration globals (set these BEFORE loading the plugin)
" ---------------------------------------------------------------------------
"   g:civic_executable    — path to the civic binary (overrides PATH lookup)
"   g:civic_no_mappings   — set to 1 to disable default key mappings
"   g:civic_auto_build    — set to 1 to auto-build on BufWritePost (default: 0)
"   g:civic_auto_lsp      — set to 1 to auto-start LSP on BufRead/BufNewFile (default: 0)
" ---------------------------------------------------------------------------

" ---------------------------------------------------------------------------
" Commands
" ---------------------------------------------------------------------------

" :CivicBuild — Build the current project or single .cv file
command! -nargs=0 -bar CivicBuild call civic#Build()

" :CivicRun — Build and run in a terminal split
command! -nargs=0 -bar CivicRun call civic#Run()

" :CivicTest — Run the test suite
command! -nargs=0 -bar CivicTest call civic#Test()

" :CivicEmitIR — Emit LLVM IR for the current project/file
command! -nargs=0 -bar CivicEmitIR call civic#EmitIR()

" :CivicCheck — Quick syntax/semantic check (no binary output)
command! -nargs=0 -bar CivicCheck call civic#Check()

" :CivicNew — Create a new Civic project
command! -nargs=0 -bar CivicNew call civic#New()

" :CivicInfo — Show plugin and project information
command! -nargs=0 -bar CivicInfo call civic#Info()

" :CivicLspStart — Start the Civic LSP server (requires nvim-lspconfig or vim-lsp)
command! -nargs=0 -bar CivicLspStart call civic#lsp#Start()

" :CivicLspStop — Stop the Civic LSP server
command! -nargs=0 -bar CivicLspStop call civic#lsp#Stop()

" :CivicLspRestart — Restart the Civic LSP server
command! -nargs=0 -bar CivicLspRestart call civic#lsp#Restart()

" ---------------------------------------------------------------------------
" Default key mappings  (disable with  :let g:civic_no_mappings = 1)
" ---------------------------------------------------------------------------
if !exists("g:civic_no_mappings")
  " Build / Check
  nnoremap <silent> <leader>cb :CivicBuild<CR>
  vnoremap <silent> <leader>cb :<C-u>CivicBuild<CR>
  " Run
  nnoremap <silent> <leader>cr :CivicRun<CR>
  vnoremap <silent> <leader>cr :<C-u>CivicRun<CR>
  " Test
  nnoremap <silent> <leader>ct :CivicTest<CR>
  vnoremap <silent> <leader>ct :<C-u>CivicTest<CR>
  " Quick check (syntax/semantic)
  nnoremap <silent> <leader>cc :CivicCheck<CR>
  vnoremap <silent> <leader>cc :<C-u>CivicCheck<CR>
  " Emit IR
  nnoremap <silent> <leader>ci :CivicEmitIR<CR>
  vnoremap <silent> <leader>ci :<C-u>CivicEmitIR<CR>
  " Info
  nnoremap <silent> <leader>cI :CivicInfo<CR>
  " LSP
  nnoremap <silent> <leader>cl :CivicLspStart<CR>
endif

" ---------------------------------------------------------------------------
" Optional: auto-build on save
" ---------------------------------------------------------------------------
if get(g:, 'civic_auto_build', 0)
  augroup CivicAutoBuild
    autocmd!
    autocmd BufWritePost *.cv if &filetype ==# 'civic' | call civic#Check() | endif
  augroup END
endif

" ---------------------------------------------------------------------------
" Set the filetype for .cv files automatically (belt-and-suspenders with
" ftdetect/civic.vim — helps when ftdetect/ is not on the runtime path)
" ---------------------------------------------------------------------------
if !has('nvim')
  augroup CivicFiletype
    autocmd!
    autocmd BufNewFile,BufRead *.cv setfiletype civic
  augroup END
endif

" vim: set ft=vim et sw=2 sts=2:
