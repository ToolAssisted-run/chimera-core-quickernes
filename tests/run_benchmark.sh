#!/bin/bash

# Tests excluded from the benchmark because they crash, time out, or diverge
# on quickerNES (kept in sync with disabledTestSet in tests/meson.build).
excludedTests=(
  "castlevania3.playaround.test"          # segfault: mapper 5 (MMC5) not registered
  "novaTheSquirrel.anyPercent.test"       # segfault (works on quickNES)
  "ironSword.anyPercent.test"             # hash mismatch vs quickNES
  "rcProAmII.race1.test"                  # hash mismatch vs quickNES
  "superOffroad.anyPercent.test"          # hash mismatch vs quickNES
  "nigelMansell.anyPercent.test"          # timeout (>120s)
  "saintSeiyaKanketsuHen.anyPercent.test" # timeout (>120s)
  # The Arkanoid tests abort unless the build was configured with
  # -DenableArkanoidInputs=true (off by default), so skip them here too.
  "arkanoid.arkNESController.test"        # requires enableArkanoidInputs build option
  "arkanoid2.arkFamicomController.test"   # requires enableArkanoidInputs build option
)

# Finding all test scripts
testScriptList=`find . -type f -name "*.test"`

# Iterating over the scripts
for script in ${testScriptList}; do

  # Getting filename
  fileName=`basename ${script}`

  # Skipping excluded tests
  skip=false
  for excluded in "${excludedTests[@]}"; do
    if [ "${fileName}" = "${excluded}" ]; then skip=true; break; fi
  done
  if [ "${skip}" = true ]; then
    echo "[] Skipping excluded test: ${fileName}"
    continue
  fi

  # Running script on quickerNES.
  # Using the 'Rerecord' cycle (advance + state load/save per frame) since the
  # TAS/re-recording use case depends on state save/load performance too, so it
  # belongs in the timing.
  ../build/quickerNESTester ${fileName} --cycleType Rerecord

  # Running script on quickNES
  ../build/quickNESTester ${fileName} --cycleType Rerecord

done

