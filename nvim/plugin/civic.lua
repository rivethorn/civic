-- plugin/civic.lua — bootstrap the Civic plugin.
-- Runs before user config for package-manager installs; user setup() calls
-- (possibly with opts) merge into the defaults registered here.
if vim.g.loaded_civic then
  return
end
vim.g.loaded_civic = true

require('civic').setup()
