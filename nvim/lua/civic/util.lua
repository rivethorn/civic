-- lua/civic/util.lua — path helpers shared by the plugin.

local M = {}

local civic_root_cache

--- Discover the Civic root the same way the compiler does (main.cpp):
--- CIVIC_ROOT, then candidates around the executable that contain stdlib/
--- plus a CMakeLists.txt or .civic-root marker.
function M.civic_root()
  if civic_root_cache then
    return civic_root_cache
  end
  local configured = vim.env.CIVIC_ROOT
  if configured and configured ~= '' then
    if vim.fn.isdirectory(configured .. '/stdlib') == 1 then
      civic_root_cache = configured
      return civic_root_cache
    end
    return nil
  end
  local exe = require('civic').executable()
  if not exe then
    return nil
  end
  exe = vim.fn.resolve(exe)
  local bindir = vim.fs.dirname(exe)
  local prefix = vim.fs.dirname(bindir)
  local candidates = {
    prefix,
    prefix .. '/share/civic',
    bindir .. '/share/civic',
    bindir,
  }
  for _, candidate in ipairs(candidates) do
    local ok = vim.fn.isdirectory(candidate .. '/stdlib') == 1
      and (
        vim.fn.filereadable(candidate .. '/CMakeLists.txt') == 1
        or vim.fn.filereadable(candidate .. '/.civic-root') == 1
      )
    if ok then
      civic_root_cache = candidate
      return civic_root_cache
    end
  end
  return nil
end

--- includeexpr: map an #include <std/io> module name to a readable .cv path.
--- Falls back to resolving relative to the current file, then to the raw
--- name so gf reports its usual failure instead of a Lua error.
function M.module_to_path(fname)
  local path = fname
  if not path:match('%.cv$') then
    path = path .. '.cv'
  end
  local root = M.civic_root()
  if root then
    local stdlib = root .. '/stdlib/' .. path
    if vim.fn.filereadable(stdlib) == 1 then
      return stdlib
    end
  end
  local bufname = vim.api.nvim_buf_get_name(0)
  if bufname ~= '' then
    local sibling = vim.fs.dirname(bufname) .. '/' .. path
    if vim.fn.filereadable(sibling) == 1 then
      return sibling
    end
  end
  return fname
end

return M
