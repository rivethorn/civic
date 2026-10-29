-- lua/civic/health.lua — :checkhealth civic

local M = {}

function M.check()
  vim.health.start('civic')

  local civic = require('civic')
  local exe = civic.executable()
  if exe then
    vim.health.ok('executable: ' .. exe)
  else
    vim.health.error(
      'civic executable not found',
      { 'install `civic` and ensure it is on $PATH', 'or set vim.g.civic_executable / setup{executable=...}' }
    )
  end

  if vim.fn.has('nvim-0.11') == 1 then
    vim.health.ok('Neovim 0.11+ (vim.lsp.config / vim.lsp.enable available)')
  else
    vim.health.warn('Neovim 0.11+ required for automatic LSP attachment')
  end

  if exe then
    local root = vim.system({ exe, '--help' }, { text = true }):wait()
    if root.code == 0 then
      vim.health.ok('`civic --help` runs successfully')
    else
      vim.health.error('`civic --help` exited with code ' .. tostring(root.code))
    end
  end

  local util = require('civic.util')
  local civic_root = util.civic_root()
  if civic_root then
    vim.health.ok('Civic root: ' .. civic_root)
    if vim.fn.isdirectory(civic_root .. '/stdlib') == 1 then
      vim.health.ok('stdlib/ present')
    else
      vim.health.error('Civic root has no stdlib/ directory')
    end
  else
    vim.health.warn(
      'could not discover the Civic root',
      { 'set CIVIC_ROOT to a directory containing stdlib/', '#include <std/...> resolution (gf) needs it' }
    )
  end

  vim.health.start('civic: project')
  local bufname = vim.api.nvim_buf_get_name(0)
  if bufname ~= '' and bufname:match('%.cv$') then
    local root = civic.root(0)
    if root then
      vim.health.ok('project root: ' .. root)
      if vim.fn.filereadable(root .. '/civic.toml') == 1 then
        vim.health.ok('civic.toml found')
      else
        vim.health.warn('civic.toml found by marker but not readable?')
      end
    else
      vim.health.info('no civic.toml above ' .. bufname .. ' (single-file mode)')
    end
  else
    vim.health.info('current buffer is not a .cv file')
  end

  vim.health.start('civic: language server')
  local clients = vim.lsp.get_clients({ name = 'civic' })
  if #clients > 0 then
    for _, client in ipairs(clients) do
      local state = client:is_stopped() and 'stopped' or 'running'
      vim.health.ok(('client %d (%s) attached to buffer %s'):format(client.id, state, bufname))
    end
  elseif civic.config.auto_lsp then
    vim.health.info('no civic LSP client attached to this buffer')
  else
    vim.health.info('auto_lsp is disabled in setup()')
  end
end

return M
