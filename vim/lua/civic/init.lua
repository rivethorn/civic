-- lua/civic/init.lua
-- Neovim LSP setup for Civic
--
-- Usage (in your init.lua, after adding this plugin to your rtp):
--
--   require('civic').setup()
--
--   -- Or with custom options:
--   require('civic').setup({
--     on_attach = function(client, bufnr) ... end,
--   })
--
-- The Civic LSP server (`civic lsp`) provides:
--   - Text document synchronization (full sync)
--   - Completion (keywords, types, built-ins, user functions)
--   - Hover (function signatures, type descriptions)
--   - Go-to-definition (across files)
--   - Publish diagnostics (lexical, parse, semantic errors)

local M = {}

-- ---------------------------------------------------------------------------
-- Resolve the project root for a given filename
-- Walks up the directory tree looking for civic.toml
-- @param fname string Filename to start from (or current buffer)
-- @return string|nil Root directory containing civic.toml, or nil
-- ---------------------------------------------------------------------------
local function resolve_root(fname)
  fname = fname or vim.api.nvim_buf_get_name(0)
  if fname == "" then
    fname = vim.fn.getcwd()
  end

  -- Prefer lspconfig.util if available
  local ok, util = pcall(require, "lspconfig.util")
  if ok and util.root_pattern then
    local root = util.root_pattern("civic.toml")(fname)
    if root then
      return root
    end
  end

  -- Fallback: manual walk-up search
  local dir = fname
  if vim.fn.isdirectory(dir) == 0 then
    dir = vim.fn.fnamemodify(dir, ":h")
  end
  while dir ~= "/" do
    if vim.fn.filereadable(dir .. "/civic.toml") == 1 then
      return dir
    end
    dir = vim.fn.fnamemodify(dir, ":h")
  end
  return nil
end

-- ---------------------------------------------------------------------------
-- M.setup(opts) — Register the Civic LSP server
--
-- Uses nvim-lspconfig if available, otherwise falls back to the
-- built-in vim.lsp.start() client.
--
-- @param opts table Optional overrides
-- ---------------------------------------------------------------------------
function M.setup(opts)
  opts = opts or {}

  -- Resolve the civic executable
  local exec = "civic"
  if vim.fn.executable("civic") == 0 then
    exec = vim.fn.exepath("civic")
    if exec == "" then
      vim.notify("civic: 'civic' executable not found in PATH", vim.log.levels.ERROR)
      return
    end
  end

  -- Build the full configuration
  local config = {
    cmd = opts.cmd or { exec, "lsp" },
    filetypes = opts.filetypes or { "civic" },
    root_dir = opts.root_dir or resolve_root,
    init_options = opts.init_options or {},
    on_attach = opts.on_attach,
  }

  -- Capabilities
  if opts.capabilities then
    config.capabilities = opts.capabilities
  else
    config.capabilities = vim.lsp.protocol.make_client_capabilities()
  end

  -- Try nvim-lspconfig
  local ok, lspconfig = pcall(require, "lspconfig")
  if ok then
    if lspconfig.civic then
      -- Civic server already registered with lspconfig
      lspconfig.civic.setup(config)
    else
      -- Register civic as a custom server
      lspconfig.setup({
        servers = {
          civic = config,
        },
      })
    end
  else
    -- No lspconfig: use built-in vim.lsp.start
    vim.lsp.start({
      name = "civic",
      cmd = config.cmd,
      filetypes = config.filetypes,
      root_dir = config.root_dir,
      init_options = config.init_options,
      capabilities = config.capabilities,
      on_attach = config.on_attach,
    })
  end
end

-- ---------------------------------------------------------------------------
-- M.config — Default configuration (for reference)
-- ---------------------------------------------------------------------------
M.config = {
  cmd = { "civic", "lsp" },
  filetypes = { "civic" },
  root_dir = resolve_root,
}

-- ---------------------------------------------------------------------------
-- M.status() — Print LSP client status for the current buffer
-- ---------------------------------------------------------------------------
function M.status()
  local clients = vim.lsp.get_clients({ name = "civic" })
  if #clients == 0 then
    print("Civic LSP: not running")
  else
    for _, client in ipairs(clients) do
      local root = client.config.root_dir
      if type(root) == "function" then
        root = root(vim.api.nvim_buf_get_name(0))
      end
      print("Civic LSP: running (root=" .. tostring(root) .. ")")
    end
  end
end

return M
