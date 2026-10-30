-- lua/civic/init.lua — Civic toolchain integration for Neovim.
--
-- Design notes (v2 rewrite):
--   * No shell strings: every external command runs through |vim.system()|
--     with an argv table and an explicit working directory, so quoting and
--     the user's $SHELL can never break a command line.
--   * Everything is async; results land in the quickfix list or a scratch
--     buffer, and the UI is only touched from the main loop.
--   * The language server attaches automatically through |vim.lsp.config()|
--     and |vim.lsp.enable()| (Neovim 0.11+); lspconfig is not required.
--   * Buffer-local settings and keymaps live in ftplugin/civic.lua.

local M = {}

M.config = {
  executable = nil,         -- override the `civic` binary; also vim.g.civic_executable
  auto_lsp = true,          -- start `civic lsp` for .cv buffers
  auto_check = vim.g.civic_auto_build == 1,  -- run CivicCheck on save of a .cv file
  format_on_save = false,   -- reformat the buffer before every save of a .cv file
  keymaps = vim.g.civic_no_mappings == nil or vim.g.civic_no_mappings == 0,
  open_quickfix = 'auto',   -- 'auto' (only with errors) | 'always' | 'never'
}

--- Resolve the civic executable: config, then vim.g, then $PATH.
function M.executable()
  local configured = M.config.executable or vim.g.civic_executable
  if type(configured) == 'string' and configured ~= '' then
    return configured
  end
  if vim.fn.executable('civic') == 1 then
    return vim.fn.exepath('civic')
  end
  return nil
end

--- Project root for a buffer: nearest ancestor directory with civic.toml.
function M.root(bufnr)
  return vim.fs.root(bufnr or 0, 'civic.toml')
end

local function notify_error(msg)
  vim.notify('civic: ' .. msg, vim.log.levels.ERROR)
end

-- ---------------------------------------------------------------------------
-- Command execution
-- ---------------------------------------------------------------------------

-- Run `civic` with `args` (argv table). opts.cwd sets the working directory.
-- on_exit(lines, code) receives merged stdout/stderr lines on the main loop.
local function civic_run(args, opts, on_exit)
  opts = opts or {}
  local exe = M.executable()
  if not exe then
    notify_error('executable not found; set vim.g.civic_executable or config.executable')
    return
  end
  local argv = { exe }
  vim.list_extend(argv, args)
  local sysopts = { text = true }
  if opts.cwd then
    sysopts.cwd = opts.cwd
  end
  local ok, obj = pcall(vim.system, argv, sysopts, function(out)
    local lines = {}
    for _, stream in ipairs({ out.stdout or '', out.stderr or '' }) do
      for line in vim.gsplit(stream, '\n', { trimempty = true }) do
        lines[#lines + 1] = line
      end
    end
    vim.schedule(function()
      on_exit(lines, out.code or -1)
    end)
  end)
  if not ok then
    vim.schedule(function()
      on_exit({
        ('failed to start %s: %s'):format(table.concat(argv, ' '), tostring(obj)),
      }, 1)
    end)
  end
end

-- Compiler diagnostics look like: /path/file.cv:5:13: error: message
local diagnostic_pattern = '^(.-):(%d+):(%d+): error: (.*)$'
local fail_pattern = '^FAIL (.*)$'

--- Fill the quickfix list from compiler output lines; returns the items.
local function fill_quickfix(lines, title)
  local items = {}
  for _, line in ipairs(lines) do
    local path, lnum, col, msg = line:match(diagnostic_pattern)
    if path then
      items[#items + 1] = {
        filename = path,
        lnum = tonumber(lnum),
        col = tonumber(col),
        text = msg,
        type = 'E',
      }
    else
      local failed = line:match(fail_pattern)
      if failed then
        -- "FAIL path (exit N)" or "FAIL path: error ..." — keep the path
        local file = failed:match('^(%S+)') or failed
        items[#items + 1] = { filename = file, lnum = 1, text = line, type = 'E' }
      end
    end
  end
  vim.fn.setqflist({}, 'r', { items = items, title = title or 'civic' })
  return items
end

local function maybe_open_quickfix(items)
  local mode = M.config.open_quickfix
  if mode == 'never' then
    return
  end
  if mode == 'always' or #items > 0 then
    vim.cmd('copen')
  end
end

-- ---------------------------------------------------------------------------
-- Scratch output buffers
-- ---------------------------------------------------------------------------

--- Show `lines` in a reusable named scratch split. ft sets the filetype.
local function scratch(name, lines, ft)
  local buf = vim.fn.bufnr(name)
  if buf == -1 then
    buf = vim.api.nvim_create_buf(false, true)
    vim.api.nvim_buf_set_name(buf, name)
  end
  vim.bo[buf].buftype = 'nofile'
  vim.bo[buf].bufhidden = 'hide'
  vim.bo[buf].swapfile = false
  vim.bo[buf].modifiable = true
  vim.api.nvim_buf_set_lines(buf, 0, -1, false, lines)
  vim.bo[buf].modifiable = false
  if ft then
    vim.bo[buf].filetype = ft
  end
  -- Reuse a window already showing the buffer; otherwise a bottom split.
  if vim.fn.win_findbuf(buf)[1] then
    vim.api.nvim_set_current_win(vim.fn.win_findbuf(buf)[1])
  else
    vim.cmd('botright ' .. math.max(math.floor(vim.o.lines / 3), 8) .. 'split')
    vim.api.nvim_win_set_buf(0, buf)
  end
  vim.keymap.set('n', 'q', '<cmd>close<CR>', { buffer = buf, silent = true, desc = 'Close output' })
  return buf
end

-- ---------------------------------------------------------------------------
-- Target resolution
-- ---------------------------------------------------------------------------

-- Returns { cwd, target_args, label } for the current buffer:
-- inside a project (civic.toml found) use the manifest, else the current file.
local function target()
  local root = M.root(0)
  if root then
    return {
      cwd = root,
      manifest_args = { '--manifest-path', root .. '/civic.toml' },
      entry = root .. '/src/main.cv',
      label = vim.fs.basename(root),
    }
  end
  local file = vim.api.nvim_buf_get_name(0)
  if file == '' or file:sub(-3) ~= '.cv' then
    return nil
  end
  return { cwd = vim.fs.dirname(file), file = file, label = vim.fs.basename(file) }
end

local function require_target(action)
  local t = target()
  if not t then
    notify_error(action .. ': open a .cv file or run inside a project')
    return nil
  end
  return t
end

-- ---------------------------------------------------------------------------
-- Commands
-- ---------------------------------------------------------------------------

function M.build(opts)
  local t = require_target('build')
  if not t then return end
  local args = { 'build' }
  if t.manifest_args then
    vim.list_extend(args, t.manifest_args)
    vim.list_extend(args, opts and opts.fargs or {})
  else
    args[#args + 1] = t.file
  end
  civic_run(args, { cwd = t.cwd }, function(lines, code)
    local items = fill_quickfix(lines, 'civic build')
    if code == 0 then
      vim.notify('civic: built ' .. t.label, vim.log.levels.INFO)
    else
      local detail = #items > 0 and (#items .. ' error(s)') or (lines[1] or 'unknown failure')
      notify_error('build failed: ' .. detail)
      maybe_open_quickfix(items)
    end
  end)
end

--- Split command-line args for `civic run`: everything after an explicit `--`
--- (or everything, unless it is a known civic flag) goes to the program.
local function split_run_args(fargs)
  local cli_args, program_args, seen_separator = {}, {}, false
  for _, arg in ipairs(fargs or {}) do
    if arg == '--' then
      seen_separator = true
    elseif not seen_separator and arg == '--release' then
      cli_args[#cli_args + 1] = arg
    else
      program_args[#program_args + 1] = arg
    end
  end
  return cli_args, program_args
end

function M.run(opts)
  local t = require_target('run')
  if not t then return end
  local exe = M.executable()
  if not exe then
    notify_error('executable not found; set vim.g.civic_executable')
    return
  end
  local argv = { exe, 'run' }
  local cli_args, program_args = split_run_args(opts and opts.fargs)
  vim.list_extend(argv, cli_args)
  if t.manifest_args then
    vim.list_extend(argv, t.manifest_args)
  else
    argv[#argv + 1] = t.file
  end
  if #program_args > 0 then
    argv[#argv + 1] = '--'
    vim.list_extend(argv, program_args)
  end
  -- `:new` (not `:split`): termopen() reuses the current buffer, so we must
  -- give it a *fresh* buffer or it would convert the source buffer itself.
  vim.cmd('botright ' .. math.max(math.floor(vim.o.lines / 3), 8) .. 'new')
  local buf = vim.api.nvim_get_current_buf()
  local ok, err = pcall(vim.fn.termopen, argv, { cwd = t.cwd })
  if not ok then
    vim.api.nvim_buf_delete(buf, { force = true })
    notify_error('failed to start terminal: ' .. tostring(err))
    return
  end
  vim.bo[buf].buflisted = false
  vim.bo[buf].filetype = 'civic-run'
  vim.keymap.set('n', 'q', '<cmd>close<CR>', { buffer = buf, silent = true, desc = 'Close run output' })
  vim.cmd('normal! G')
  vim.cmd('startinsert!')
end

--- Working directory for `civic test`: tests/ are discovered relative to cwd,
--- so prefer a directory that actually contains tests/ — the project root,
--- then the current working directory, then the file's own directory.
local function tests_cwd()
  local t = target()
  local candidates = {}
  if t then
    candidates[#candidates + 1] = t.cwd
  end
  candidates[#candidates + 1] = vim.fn.getcwd()
  for _, dir in ipairs(candidates) do
    if vim.fn.isdirectory(dir .. '/tests') == 1 then
      return dir
    end
  end
  return (t and t.cwd) or vim.fn.getcwd()
end

function M.test(opts)
  local cwd = tests_cwd()
  local args = { 'test' }
  vim.list_extend(args, opts and opts.fargs or {})
  civic_run(args, { cwd = cwd }, function(lines, code)
    local items = fill_quickfix(lines, 'civic test')
    scratch('civic://tests', lines, 'text')
    -- scratch() leaves us in the output window; highlight PASS/FAIL there.
    -- clearmatches() keeps repeated runs from stacking duplicate matches.
    pcall(vim.fn.clearmatches)
    pcall(vim.fn.matchadd, 'CivicTestPass', '\\<PASS\\>')
    pcall(vim.fn.matchadd, 'CivicTestFail', '\\<FAIL\\>')
    if code == 0 then
      vim.notify('civic: all tests passed', vim.log.levels.INFO)
    else
      notify_error('tests failed')
      maybe_open_quickfix(items)
    end
  end)
end

function M.check()
  local t = require_target('check')
  if not t then return end
  local source = t.file or t.entry
  if vim.fn.filereadable(source) ~= 1 then
    notify_error('check: entry point not found: ' .. source)
    return
  end
  local tmp = vim.fn.tempname() .. '.ll'
  civic_run({ 'emit-ir', source, '-o', tmp }, { cwd = t.cwd }, function(lines, code)
    pcall(vim.fn.delete, tmp)
    local items = fill_quickfix(lines, 'civic check')
    if code == 0 then
      vim.notify('civic: no errors', vim.log.levels.INFO)
    else
      local detail = #items > 0 and (#items .. ' error(s)') or (lines[1] or 'unknown failure')
      notify_error('check failed: ' .. detail)
      maybe_open_quickfix(items)
    end
  end)
end

--- Format the current .cv buffer with the canonical `civic fmt` layout.
---
--- Prefers the attached language server (works on unsaved contents, without
--- a disk round trip); otherwise formats a temp snapshot of the buffer with
--- the CLI and applies the result. Provides a `civic fmt` keymap fallback in
--- either mode.
function M.format(bufnr)
  bufnr = bufnr or 0
  local path = vim.api.nvim_buf_get_name(bufnr)
  if path == '' or path:sub(-3) ~= '.cv' then
    notify_error('fmt: the current buffer is not a .cv file')
    return
  end
  -- 1) Language server: textDocument/formatting on the buffer contents.
  for _, client in ipairs(vim.lsp.get_clients({ bufnr = bufnr, name = 'civic' })) do
    if client.supports_method and client:supports_method('textDocument/formatting') then
      vim.lsp.buf.format({ bufnr = bufnr, async = false })
      vim.notify('civic: formatted buffer (lsp)', vim.log.levels.INFO)
      return
    end
  end
  -- 2) CLI: format a temp copy of the buffer text and splice it back.
  local exe = M.executable()
  if not exe then
    notify_error('executable not found; cannot format')
    return
  end
  local tmp = vim.fn.tempname() .. '.cv'
  vim.fn.writefile(vim.api.nvim_buf_get_lines(bufnr, 0, -1, false), tmp)
  local out = vim.system({ exe, 'fmt', tmp }, { text = true }):wait()
  local formatted = vim.fn.readfile(tmp)
  pcall(vim.fn.delete, tmp)
  if out.code ~= 0 then
    notify_error('fmt failed: ' .. vim.trim(out.stderr or 'no output'))
    return
  end
  local view = vim.fn.winsaveview()
  vim.api.nvim_buf_set_lines(bufnr, 0, -1, false, formatted)
  vim.fn.winrestview(view)
  vim.notify('civic: formatted buffer', vim.log.levels.INFO)
end

function M.emit_ir()
  local t = require_target('emit-ir')
  if not t then return end
  local source = t.file or t.entry
  if vim.fn.filereadable(source) ~= 1 then
    notify_error('emit-ir: entry point not found: ' .. source)
    return
  end
  local tmp = vim.fn.tempname() .. '.ll'
  civic_run({ 'emit-ir', source, '-o', tmp }, { cwd = t.cwd }, function(lines, code)
    if code ~= 0 then
      pcall(vim.fn.delete, tmp)
      local items = fill_quickfix(lines, 'civic emit-ir')
      notify_error('emit-ir failed (' .. #items .. ' error(s))')
      maybe_open_quickfix(items)
      return
    end
    local ir = vim.fn.readfile(tmp)
    pcall(vim.fn.delete, tmp)
    scratch('civic://ir', ir, 'llvm')
  end)
end

function M.new(opts)
  local name = opts and opts.args or ''
  if name == '' then
    name = vim.fn.input('New Civic project name: ')
  end
  if name == '' then
    return
  end
  civic_run({ 'new', name }, { cwd = vim.fn.getcwd() }, function(lines, code)
    if code ~= 0 then
      notify_error('new failed: ' .. (lines[1] or 'unknown error'))
      return
    end
    vim.notify(lines[#lines] or ('created project ' .. name), vim.log.levels.INFO)
    local entry = vim.fs.joinpath(vim.fn.fnamemodify(name, ':p'), 'src', 'main.cv')
    if vim.fn.filereadable(entry) == 1 then
      vim.cmd('edit ' .. vim.fn.fnameescape(entry))
    end
  end)
end

function M.info()
  local exe = M.executable()
  local lines = {
    'Civic plugin info',
    '',
    exe and ('executable: ' .. exe) or 'executable: NOT FOUND (install civic or set vim.g.civic_executable)',
    'CIVIC_ROOT: ' .. (vim.env.CIVIC_ROOT or '(unset)'),
  }
  local root = M.root(0)
  if root then
    lines[#lines + 1] = 'project root: ' .. root
    local manifest = root .. '/civic.toml'
    if vim.fn.filereadable(manifest) == 1 then
      lines[#lines + 1] = 'manifest:'
      for _, l in ipairs(vim.fn.readfile(manifest)) do
        lines[#lines + 1] = '  ' .. l
      end
    end
  else
    lines[#lines + 1] = 'project root: (none — no civic.toml above this file)'
  end
  local clients = vim.lsp.get_clients({ name = 'civic' })
  lines[#lines + 1] = 'lsp clients: ' .. (#clients > 0 and #clients or 'none')
  vim.notify(table.concat(lines, '\n'), vim.log.levels.INFO)
end

-- ---------------------------------------------------------------------------
-- LSP
-- ---------------------------------------------------------------------------

local function lsp_config(exe)
  return {
    cmd = { exe, 'lsp' },
    filetypes = { 'civic' },
    root_markers = { 'civic.toml' },
  }
end

function M.lsp_stop()
  local clients = vim.lsp.get_clients({ name = 'civic' })
  for _, client in ipairs(clients) do
    client:stop()
  end
  vim.notify('civic: stopped ' .. #clients .. ' lsp client(s)', vim.log.levels.INFO)
end

--- (Re)apply the LSP config and attach to every already-open civic buffer.
--- Complements vim.lsp.enable(), which only reacts to future FileType events.
function M.lsp_start()
  local exe = M.executable()
  if not exe then
    notify_error('executable not found; cannot start lsp')
    return
  end
  if not (vim.lsp.config and vim.lsp.enable) then
    notify_error('automatic LSP needs Neovim 0.11+')
    return
  end
  vim.lsp.config.civic = lsp_config(exe)
  vim.lsp.enable('civic') -- re-enables and re-fires FileType for open buffers
  vim.notify('civic: lsp started', vim.log.levels.INFO)
end

function M.lsp_restart()
  local exe = M.executable()
  if not exe then
    notify_error('executable not found; cannot restart lsp')
    return
  end
  for _, client in ipairs(vim.lsp.get_clients({ name = 'civic' })) do
    client:stop()
  end
  -- Re-create the config (cmd may have changed) and re-enable.
  vim.lsp.config.civic = lsp_config(exe)
  vim.lsp.enable('civic')
  vim.notify('civic: lsp restarted', vim.log.levels.INFO)
end

local function setup_lsp()
  if not M.config.auto_lsp then
    return
  end
  if not (vim.lsp.config and vim.lsp.enable) then
    vim.notify_once(
      'civic: automatic LSP needs Neovim 0.11+; run :CivicLspRestart manually',
      vim.log.levels.WARN
    )
    return
  end
  local exe = M.executable()
  if not exe then
    return -- stay quiet at startup; :CivicInfo explains
  end
  vim.lsp.config.civic = lsp_config(exe)
  vim.lsp.enable('civic')
end

--- Apply buffer-local options and keymaps; called from ftplugin/civic.lua.
function M.buffer_setup(bufnr)
  if M.config.keymaps then
    local map = function(lhs, rhs, desc)
      vim.keymap.set('n', lhs, rhs, { buffer = bufnr, silent = true, desc = desc })
    end
    map('<leader>cb', M.build, 'Civic: build')
    map('<leader>cr', M.run, 'Civic: run')
    map('<leader>ct', M.test, 'Civic: test')
    map('<leader>cc', M.check, 'Civic: check')
    map('<leader>cf', M.format, 'Civic: format')
    map('<leader>ci', M.emit_ir, 'Civic: emit LLVM IR')
    map('<leader>cl', M.lsp_restart, 'Civic: restart LSP')
    map('<leader>cI', M.info, 'Civic: info')
  end
end

function M.setup(opts)
  M.config = vim.tbl_deep_extend('force', M.config, opts or {})
  -- Mirror the bootstrap guard so plugin/civic.lua and a user-initiated
  -- setup() agree on whether the plugin is live.
  vim.g.loaded_civic = true

  local group = vim.api.nvim_create_augroup('civic-plugin', { clear = true })
  if type(M.config.on_attach) == 'function' then
    vim.api.nvim_create_autocmd('LspAttach', {
      group = group,
      callback = function(ev)
        local client = vim.lsp.get_client_by_id(ev.data.client_id)
        if client and client.name == 'civic' then
          pcall(M.config.on_attach, client, ev.buf)
        end
      end,
    })
  end
  if M.config.auto_check then
    vim.api.nvim_create_autocmd('BufWritePost', {
      group = group,
      pattern = '*.cv',
      callback = function()
        M.check() -- checks the buffer that was just written
      end,
    })
  end
  if M.config.format_on_save then
    vim.api.nvim_create_autocmd('BufWritePre', {
      group = group,
      pattern = '*.cv',
      callback = function()
        -- Runs synchronously so the write lands on formatted text.
        M.format()
      end,
    })
  end

  local command = vim.api.nvim_create_user_command
  command('CivicBuild', M.build, { nargs = '*', desc = 'Build the Civic project or file' })
  command('CivicRun', M.run, { nargs = '*', desc = 'Build and run (args are passed to the program)' })
  command('CivicTest', M.test, { nargs = '*', desc = 'Run civic test' })
  command('CivicCheck', M.check, { desc = 'Type-check without linking' })
  command('CivicFmt', function(opts)
    M.format(opts and opts.fargs and tonumber(opts.fargs[1]) or 0)
  end, { nargs = '?', desc = 'Format the buffer with civic fmt' })
  command('CivicEmitIR', M.emit_ir, { desc = 'Show generated LLVM IR' })
  command('CivicNew', M.new, { nargs = '?', desc = 'Create a new Civic project' })
  command('CivicInfo', M.info, { desc = 'Show Civic plugin information' })
  command('CivicLspStart', M.lsp_start, { desc = 'Start the Civic language server' })
  command('CivicLspStop', M.lsp_stop, { desc = 'Stop the Civic language server' })
  command('CivicLspRestart', M.lsp_restart, { desc = 'Restart the Civic language server' })

  setup_lsp()
  return M
end

-- Highlight groups for test output (defined early so ftplugin can rely on them).
vim.api.nvim_set_hl(0, 'CivicTestPass', { link = 'DiagnosticOk', default = true })
vim.api.nvim_set_hl(0, 'CivicTestFail', { link = 'DiagnosticError', default = true })

return M
