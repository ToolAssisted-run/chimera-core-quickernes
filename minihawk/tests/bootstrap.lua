-- miniHawk witness harness bootstrap: exit immediately so EmuHawk writes a
-- fresh default config to the path given via --config (run-level-b harness
-- settings are then enforced on that file by the driver).
client.exit()
