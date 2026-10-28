-- lua/civic/init.lua — Civic plugin for Neovim (Lua only)
local M = {}

local cfg = {
  executable = nil,
  no_mappings = false,
  auto_build = false,
}

local function exec()
  if cfg.executable then return cfg.executable end
  if vim.fn.executable('civic') == 1 then return 'civic' end
  local p = vim.fn.exepath('civic')
  return p ~= '' and p or nil
end

local function has_exec() return exec() ~= nil end

local function find_root(start)
  start = start or vim.api.nvim_buf_get_name(0)
  if start == '' then start = vim.fn.getcwd() end
  local dir = vim.fn.isdirectory(start) == 1 and start or vim.fn.fnamemodify(start, ':h')
  while dir ~= '/' do
    if vim.fn.filereadable(dir .. '/civic.toml') == 1 then return dir end
    local p = vim.fn.fnamemodify(dir, ':h')
    if p == dir then break end
    dir = p
  end
  return nil
end

local function cur_file() return vim.fn.expand('%:p') end

local function run(cmd, cwd)
  if cwd then cmd = 'cd ' .. vim.fn.shellescape(cwd) .. ' && ' .. cmd end
  return vim.split(vim.fn.system(cmd), '\n')
end

local function qf(output)
  local e = {}
  for _, l in ipairs(output) do
    l = vim.trim(l)
    if l ~= '' then
      local t = 'E'
      if l:match('[Ww]arning') then t = 'W' elseif l:match('[Nn]ote') then t = 'I' end
      table.insert(e, { text = l, type = t })
    end
  end
  vim.fn.setqflist({}, ' ', e)
  if #e > 0 then vim.cmd('copen') end
end

local function scratch(name, output, ft)
  if vim.fn.bufexists(name) == 1 then vim.cmd('bdelete! ' .. name) end
  vim.cmd('new ' .. name)
  vim.api.nvim_buf_set_lines(0, 0, -1, false, output)
  vim.bo[0].buftype = 'nofile'
  vim.bo[0].bufhidden = 'hide'
  vim.bo[0].noswapfile = true
  vim.bo[0].readonly = true
  vim.bo[0].filetype = ft
  vim.keymap.set('n', 'q', ':q<CR>', { buffer = true, silent = true })
end

function M.build()
  if not has_exec() then vim.notify('civic: executable not found', vim.log.levels.ERROR) return end
  local root = find_root()
  local cmd
  if root then
    cmd = exec() .. ' build --manifest-path ' .. vim.fn.shellescape(root .. '/civic.toml')
  else
    local f = cur_file()
    if vim.fn.fnamemodify(f, ':e') ~= 'cv' then
      vim.notify('civic build: not a .cv file', vim.log.levels.ERROR) return
    end
    cmd = exec() .. ' build ' .. vim.fn.shellescape(f)
  end
  local out = run(cmd, root or '')
  qf(out)
  for _, l in ipairs(out) do if l:match('error:') then return end end
  vim.notify(root and 'built (project)' or 'built ' .. vim.fn.fnamemodify(cur_file(), ':t'))
end

function M.run()
  if not has_exec() then vim.notify('civic: executable not found', vim.log.levels.ERROR) return end
  local root = find_root()
  local args = root and { '--manifest-path', root .. '/civic.toml' } or { cur_file() }
  if not root and vim.fn.fnamemodify(cur_file(), ':e') ~= 'cv' then
    vim.notify('civic run: not a .cv file', vim.log.levels.ERROR) return
  end
  local cmd = exec() .. ' run'
  for _, a in ipairs(args) do cmd = cmd .. ' ' .. vim.fn.shellescape(a) end
  local cwd = root or ''
  if cwd then cmd = 'cd ' .. vim.fn.shellescape(cwd) .. ' && ' .. cmd end
  local h = math.max(math.floor(vim.fn.winheight(0) / 2), 10)
  vim.cmd('botright ' .. h .. 'new')
  vim.cmd('terminal ' .. cmd)
  vim.bo[0].nonumber = true
  vim.bo[0].relativenumber = false
  vim.bo[0].buflisted = true
  vim.bo[0].filetype = 'terminal'
  vim.keymap.set('n', 'q', ':q<CR>', { buffer = true, silent = true })
end

function M.test()
  if not has_exec() then vim.notify('civic: executable not found', vim.log.levels.ERROR) return end
  local root = find_root()
  local out = run(exec() .. ' test', root or vim.fn.getcwd())
  scratch('*Civic Test Output*', out, 'civic')
  vim.fn.matchadd('CivicTestPass', 'PASS')
  vim.fn.matchadd('CivicTestFail', 'FAIL')
end

function M.emit_ir()
  if not has_exec() then vim.notify('civic: executable not found', vim.log.levels.ERROR) return end
  local root = find_root()
  local f
  if root then
    f = root .. '/src/main.cv'
  else
    f = cur_file()
    if vim.fn.fnamemodify(f, ':e') ~= 'cv' then
      vim.notify('civic emit-ir: not a .cv file', vim.log.levels.ERROR) return
    end
  end
  local out = run(exec() .. ' emit-ir ' .. vim.fn.shellescape(f), root or '')
  scratch('*Civic LLVM IR*', out, 'llvm')
end

function M.check()
  if not has_exec() then vim.notify('civic: executable not found', vim.log.levels.ERROR) return 1 end
  local root = find_root()
  local f
  if root then
    f = root .. '/src/main.cv'
  else
    f = cur_file()
    if vim.fn.fnamemodify(f, ':e') ~= 'cv' then
      vim.notify('civic check: not a .cv file', vim.log.levels.ERROR) return 1
    end
  end
  local tmp = vim.fn.tempname()
  local out = run(exec() .. ' emit-ir ' .. vim.fn.shellescape(f) .. ' -o ' .. vim.fn.shellescape(tmp), root or '')
  if vim.fn.filereadable(tmp) == 1 then vim.fn.delete(tmp) end
  qf(out)
  for _, l in ipairs(out) do if l:match('error:') then return 1 end end
  vim.notify('Civic: no errors')
  return 0
end

function M.new()
  if not has_exec() then vim.notify('civic: executable not found', vim.log.levels.ERROR) return end
  local name = vim.fn.input('Create Civic project: ', '', 'file')
  if name == '' then vim.notify('cancelled') return end
  local out = run(exec() .. ' new ' .. vim.fn.shellescape(name))
  for _, l in ipairs(out) do vim.notify(l) end
  local mf = vim.fn.fnamemodify(name, ':p') .. '/src/main.cv'
  if vim.fn.filereadable(mf) == 1 then
    vim.cmd('edit ' .. vim.fn.fnameescape(mf))
    vim.notify('Opened ' .. mf)
  end
end

function M.info()
  local e = exec()
  print('Civic Plugin Info')
  print('')
  print(e and ('Executable: ' .. e) or 'Executable: NOT FOUND')
  if not e then print('  Install civic or set g:civic_executable') end
  print('')
  local root = find_root()
  if root then
    print('Project root: ' .. root)
    local manifest = root .. '/civic.toml'
    if vim.fn.filereadable(manifest) == 1 then
      for _, l in ipairs(vim.fn.readfile(manifest)) do print('  ' .. l) end
    end
  else
    print('Project root: (none — no civic.toml found)')
  end
end

function M.lsp_start()
  if not has_exec() then vim.notify('civic: executable not found', vim.log.levels.ERROR) return end
  local root = find_root()
  local root_dir = root or vim.fn.fnamemodify(vim.api.nvim_buf_get_name(0), ':p:h')
  local cap = vim.lsp.protocol.make_client_capabilities()
  local conf = {
    cmd = { exec(), 'lsp' },
    filetypes = { 'civic' },
    root_dir = root_dir,
    capabilities = cap,
  }
  local ok, lspconfig = pcall(require, 'lspconfig')
  if ok then
    if lspconfig.civic then
      lspconfig.civic.setup(conf)
    else
      lspconfig.setup({ servers = { civic = conf } })
    end
  else
    vim.lsp.start({ name = 'civic', cmd = conf.cmd, filetypes = conf.filetypes,
      root_dir = conf.root_dir, capabilities = conf.capabilities })
  end
  vim.notify('Civic LSP started')
end

function M.lsp_stop()
  local clients = vim.lsp.get_clients({ name = 'civic' })
  if #clients == 0 then print('Civic LSP: no active client')
  else
    for _, c in ipairs(clients) do c:stop() end
    print('Civic LSP: stopped ' .. #clients .. ' client(s)')
  end
end

function M.lsp_restart()
  M.lsp_stop()
  vim.defer_fn(M.lsp_start, 500)
end

vim.api.nvim_create_autocmd('FileType', {
  pattern = 'civic',
  callback = function()
    vim.bo.commentstring = '// %s'
    vim.bo.comments = '://,:///,://!,b:/*,*:*,b:*/,://'
    vim.bo.suffixes = '.cv'
    vim.bo.include = '\\import'
    vim.bo.suffixesadd = '.cv'
    vim.bo.errorformat = '%f:%l:%c\\: error\\: %m'
    vim.bo.iskeyword = '@,#,$,_,1-9,-,@'
    vim.bo.matchpairs = '(:),{:},[:]'
    vim.bo.autoindent = true
    vim.bo.smartindent = true
    vim.bo.expandtab = false
    vim.bo.tabstop = 4
    vim.bo.shiftwidth = 4
    vim.bo.softtabstop = 4
    vim.bo.foldmethod = 'syntax'
    vim.bo.foldtext = ''
    vim.bo.cindent = true
    vim.bo.cinoptions = '>s,e-s,n-s'
    vim.bo.indentexpr = ''
    vim.bo.indentkeys = '0{,0},0),:,!^F,o,O,0='
  end,
})

function M.setup(opts)
  opts = opts or {}
  cfg.executable = opts.executable or vim.g.civic_executable or cfg.executable
  cfg.no_mappings = opts.no_mappings ~= nil and opts.no_mappings or vim.g.civic_no_mappings or false
  cfg.auto_build = opts.auto_build ~= nil and opts.auto_build or vim.g.civic_auto_build or false

  vim.api.nvim_create_user_command('CivicBuild', M.build, {})
  vim.api.nvim_create_user_command('CivicRun', M.run, {})
  vim.api.nvim_create_user_command('CivicTest', M.test, {})
  vim.api.nvim_create_user_command('CivicEmitIR', M.emit_ir, {})
  vim.api.nvim_create_user_command('CivicCheck', M.check, {})
  vim.api.nvim_create_user_command('CivicNew', M.new, {})
  vim.api.nvim_create_user_command('CivicInfo', M.info, {})
  vim.api.nvim_create_user_command('CivicLspStart', M.lsp_start, {})
  vim.api.nvim_create_user_command('CivicLspStop', M.lsp_stop, {})
  vim.api.nvim_create_user_command('CivicLspRestart', M.lsp_restart, {})

  if not cfg.no_mappings then
    local map = function(k, cmd)
      vim.keymap.set('n', '<leader>c' .. k, ':' .. cmd .. '<CR>', { silent = true })
    end
    map('b', 'CivicBuild')
    map('r', 'CivicRun')
    map('t', 'CivicTest')
    map('c', 'CivicCheck')
    map('i', 'CivicEmitIR')
    map('I', 'CivicInfo')
    map('l', 'CivicLspStart')
  end

  if cfg.auto_build then
    vim.api.nvim_create_autocmd('BufWritePost', { pattern = '*.cv', callback = M.check })
  end
end

return M
