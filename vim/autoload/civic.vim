" autoload/civic.vim — Core Civic plugin functions
" Part of the Civic vim/neovim plugin
"
" Provides project-root detection, executable discovery, and commands
" for interfacing with the Civic compiler toolchain.

if exists("g:loaded_civic_autoload")
  finish
endif
let g:loaded_civic_autoload = 1

" ---------------------------------------------------------------------------
" s:FindExecutable() — Locate the civic binary
"
" Priority:
"   1. g:civic_executable  (user-configured)
"   2. Environment  $CIVIC_PATH
"   3. PATH lookup  (exepath('civic'))
" ---------------------------------------------------------------------------
function! s:FindExecutable() abort
  if get(g:, 'civic_executable', '') !=# ''
    return g:civic_executable
  endif
  let env = $CIVIC_PATH
  if !empty(env) && executable(env)
    return env
  endif
  if executable('civic')
    return 'civic'
  endif
  return ''
endfunction

" ---------------------------------------------------------------------------
" civic#Executable() — Public accessor for the civic binary path
" ---------------------------------------------------------------------------
function! civic#Executable() abort
  return s:FindExecutable()
endfunction

" ---------------------------------------------------------------------------
" civic#HasExecutable() — Check if civic is available on this system
" ---------------------------------------------------------------------------
function! civic#HasExecutable() abort
  return !empty(s:FindExecutable())
endfunction

" ---------------------------------------------------------------------------
" s:FindProjectRoot([start]) — Walk up looking for civic.toml
" Returns the project root directory, or '' if none found.
" ---------------------------------------------------------------------------
function! s:FindProjectRoot(...) abort
  let start = a:0 ? a:1 : getcwd()
  let dir = start
  if !isdirectory(dir)
    let dir = fnamemodify(dir, ':h')
  endif
  while 1
    if filereadable(dir . '/civic.toml')
      return dir
    endif
    let parent = fnamemodify(dir, ':h')
    if parent ==# dir
      break
    endif
    let dir = parent
  endwhile
  return ''
endfunction

" ---------------------------------------------------------------------------
" civic#ProjectRoot() — Public accessor for project root
" ---------------------------------------------------------------------------
function! civic#ProjectRoot() abort
  return s:FindProjectRoot()
endfunction

" ---------------------------------------------------------------------------
" s:CurrentFile() — Get the current buffer's absolute file path
" ---------------------------------------------------------------------------
function! s:CurrentFile() abort
  return expand('%:p')
endfunction

" ---------------------------------------------------------------------------
" s:RunSilent(cmd, [opts]) — Run a command, return list of output lines
"   opts.cwd  — directory to run in (prepends 'cd <dir> &&')
" ---------------------------------------------------------------------------
function! s:RunSilent(cmd, ...) abort
  let opts = a:0 ? a:1 : {}
  let cmd = a:cmd
  if has_key(opts, 'cwd') && !empty(opts.cwd)
    let cmd = 'cd ' . shellescape(opts.cwd, 1) . ' && ' . cmd
  endif
  let output = system(cmd)
  return split(output, '\n')
endfunction

" ---------------------------------------------------------------------------
" s:CollectQuickfix(output) — Extract diagnostic lines from compiler output
" Returns a list of quickfix dict entries with 'text' and 'type' fields.
" ---------------------------------------------------------------------------
function! s:CollectQuickfix(output) abort
  let filtered = []
  for line in a:output
    let line = trim(line)
    if empty(line)
      continue
    endif
    let entry = {
          \ 'text': line,
          \ 'type': 'E',
          \ }
    if line =~# '\cwarning'
      let entry.type = 'W'
    elseif line =~# '\cnote'
      let entry.type = 'I'
    endif
    let filtered += [entry]
  endfor
  return filtered
endfunction

" ---------------------------------------------------------------------------
" s:ShowQuickfix(output) — Populate quickfix window with diagnostics
" ---------------------------------------------------------------------------
function! s:ShowQuickfix(output) abort
  let entries = s:CollectQuickfix(a:output)
  call setqflist([], ' ', entries)
  if !empty(entries)
    copen
  endif
endfunction

" ---------------------------------------------------------------------------
" civic#Build() — Build the current project or single file
" ---------------------------------------------------------------------------
function! civic#Build() abort
  let exec = s:FindExecutable()
  if empty(exec)
    echohl ErrorMsg | echo "civic: executable not found in PATH. Install civic or set g:civic_executable." | echohl None
    return 1
  endif

  let root = s:FindProjectRoot()
  if !empty(root)
    let cmd = exec . ' build'
    let cmd .= ' --manifest-path ' . shellescape(root . '/civic.toml', 1)
  else
    " File mode
    let file = s:CurrentFile()
    if fnamemodify(file, ':e') !=# 'cv'
      echohl ErrorMsg | echo "civic build: current buffer is not a .cv file" | echohl None
      return 1
    endif
    let cmd = exec . ' build ' . shellescape(file, 1)
  endif

  let output = s:RunSilent(cmd, {'cwd': empty(root) ? '' : root})
  call s:ShowQuickfix(output)

  " Determine success: if any 'error:' lines appear, the build failed
  for line in output
    if line =~# 'error:'
      return 1
    endif
  endfor
  if !empty(root)
    echo "built (project mode)"
  else
    echo "built " . fnamemodify(s:CurrentFile(), ':t')
  endif
  return 0
endfunction

" ---------------------------------------------------------------------------
" civic#Run() — Build and run in a terminal split
" ---------------------------------------------------------------------------
function! civic#Run() abort
  let exec = s:FindExecutable()
  if empty(exec)
    echohl ErrorMsg | echo "civic: executable not found in PATH" | echohl None
    return
  endif

  let root = s:FindProjectRoot()
  let args = []
  if !empty(root)
    let args += ['--manifest-path', root . '/civic.toml']
  else
    let file = s:CurrentFile()
    if fnamemodify(file, ':e') !=# 'cv'
      echohl ErrorMsg | echo "civic run: current buffer is not a .cv file" | echohl None
      return
    endif
    let args += [file]
  endif

  let cmd = exec . ' run'
  for arg in args
    let cmd .= ' ' . shellescape(arg, 1)
  endfor

  let term_opts = {}
  if !empty(root)
    let term_opts.cwd = root
  endif
  call s:OpenTerminal(cmd, term_opts)
endfunction

" ---------------------------------------------------------------------------
" s:OpenTerminal(cmd, opts) — Open a terminal split with given command
" Handles both Neovim and Vim 8+ terminal APIs.
" ---------------------------------------------------------------------------
function! s:OpenTerminal(cmd, opts) abort
  let lines = max([winheight(0) / 2, 10])
  let term_cmd = a:cmd
  if has_key(a:opts, 'cwd') && !empty(a:opts.cwd)
    let term_cmd = 'cd ' . shellescape(a:opts.cwd, 1) . ' && ' . term_cmd
  endif
  if has('nvim') || has('terminal')
    execute 'botright ' . lines . 'new'
    execute 'terminal ' . term_cmd
    setlocal nonumber norelativenumber
    setlocal buflisted
    setlocal filetype=terminal
    nnoremap <buffer> q :q<CR>
  else
    execute '!' . term_cmd
  endif
endfunction

" ---------------------------------------------------------------------------
" civic#Test() — Run civic tests and show results in a scratch buffer
" ---------------------------------------------------------------------------
function! civic#Test() abort
  let exec = s:FindExecutable()
  if empty(exec)
    echohl ErrorMsg | echo "civic: executable not found in PATH" | echohl None
    return
  endif

  let root = s:FindProjectRoot()
  let cwd = !empty(root) ? root : getcwd()

  let cmd = exec . ' test'
  let output = s:RunSilent(cmd, {'cwd': cwd})

  let bufname = '*Civic Test Output*'
  if bufexists(bufname)
    execute 'bdelete! ' . bufname
  endif
  execute 'new ' . bufname
  call setline(1, output)
  setlocal buftype=nofile
  setlocal bufhidden=hide
  setlocal noswapfile
  setlocal readonly
  setlocal filetype=civic
  nnoremap <buffer> q :q<CR>
  highlight link CivicTestPass MoreMsg
  highlight link CivicTestFail Error
  call matchadd('CivicTestPass', 'PASS')
  call matchadd('CivicTestFail', 'FAIL')
endfunction

" ---------------------------------------------------------------------------
" civic#EmitIR() — Emit LLVM IR and display in a scratch buffer
" ---------------------------------------------------------------------------
function! civic#EmitIR() abort
  let exec = s:FindExecutable()
  if empty(exec)
    echohl ErrorMsg | echo "civic: executable not found in PATH" | echohl None
    return
  endif

  let root = s:FindProjectRoot()
  let file = ''
  if !empty(root)
    let file = root . '/src/main.cv'
  else
    let file = s:CurrentFile()
    if fnamemodify(file, ':e') !=# 'cv'
      echohl ErrorMsg | echo "civic emit-ir: current buffer is not a .cv file" | echohl None
      return
    endif
  endif

  let cmd = exec . ' emit-ir ' . shellescape(file, 1)
  let output = s:RunSilent(cmd, {'cwd': empty(root) ? '' : root})

  let bufname = '*Civic LLVM IR*'
  if bufexists(bufname)
    execute 'bdelete! ' . bufname
  endif
  execute 'new ' . bufname
  call setline(1, output)
  setlocal buftype=nofile
  setlocal bufhidden=hide
  setlocal noswapfile
  setlocal readonly
  setlocal filetype=llvm
  nnoremap <buffer> q :q<CR>
endfunction

" ---------------------------------------------------------------------------
" civic#Check() — Quick syntax/semantic check without generating a binary
" ---------------------------------------------------------------------------
function! civic#Check() abort
  let exec = s:FindExecutable()
  if empty(exec)
    echohl ErrorMsg | echo "civic: executable not found in PATH" | echohl None
    return 1
  endif

  let root = s:FindProjectRoot()
  let file = ''
  if !empty(root)
    let file = root . '/src/main.cv'
  else
    let file = s:CurrentFile()
    if fnamemodify(file, ':e') !=# 'cv'
      echohl ErrorMsg | echo "civic check: current buffer is not a .cv file" | echohl None
      return 1
    endif
  endif

  " Use emit-ir to a temp file for validation
  let tmp = tempname()
  let cmd = exec . ' emit-ir ' . shellescape(file, 1) . ' -o ' . shellescape(tmp, 1)
  let output = s:RunSilent(cmd, {'cwd': empty(root) ? '' : root})
  if filereadable(tmp)
    call delete(tmp)
  endif
  call s:ShowQuickfix(output)

  for line in output
    if line =~# 'error:'
      return 1
    endif
  endfor
  echohl MoreMsg | echo "Civic: no errors" | echohl None
  return 0
endfunction

" ---------------------------------------------------------------------------
" civic#New() — Create a new Civic project
" ---------------------------------------------------------------------------
function! civic#New() abort
  let exec = s:FindExecutable()
  if empty(exec)
    echohl ErrorMsg | echo "civic: executable not found in PATH" | echohl None
    return
  endif

  let name = input("Create Civic project: ", "", "file")
  if empty(name)
    echo "cancelled"
    return
  endif

  let cmd = exec . ' new ' . shellescape(name, 1)
  let output = s:RunSilent(cmd)

  for line in output
    echo line
  endfor

  let main_file = fnamemodify(name, ':p') . '/src/main.cv'
  if filereadable(main_file)
    execute 'edit ' . fnameescape(main_file)
    echohl MoreMsg | echo "Opened " . main_file | echohl None
  endif
endfunction

" ---------------------------------------------------------------------------
" civic#Info() — Show information about the Civic setup
" ---------------------------------------------------------------------------
function! civic#Info() abort
  echohl Title | echo "Civic Plugin Info" | echohl None
  echo ""

  let exec = s:FindExecutable()
  if !empty(exec)
    echohl MoreMsg | echo "Executable: " . exec | echohl None
  else
    echohl ErrorMsg | echo "Executable: NOT FOUND" | echohl None
    echo "  Install civic or set g:civic_executable"
  endif
  echo ""

  let root = s:FindProjectRoot()
  if !empty(root)
    echohl MoreMsg | echo "Project root: " . root | echohl None
    let manifest = root . '/civic.toml'
    if filereadable(manifest)
      for line in readfile(manifest)
        echo "  " . line
      endfor
    endif
  else
    echo "Project root: (none — no civic.toml found)"
  endif
endfunction
