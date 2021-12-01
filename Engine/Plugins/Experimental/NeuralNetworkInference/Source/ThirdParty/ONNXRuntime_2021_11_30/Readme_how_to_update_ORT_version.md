# README

More ONNX Runtime compiling info in [onnxruntime.ai/docs/how-to/build/inferencing.html](https://www.onnxruntime.ai/docs/how-to/build/inferencing.html).
For questions, ask Gines.Hidalgo.



## Table of Contents
1. [Prerequisites](#prerequisites)
2. [Upgrading ONNX Runtime](#upgrading-onnx-runtime)
3. [Test and Debug ONNX Runtime on GitHub](#test-and-debug-onnx-runtime-on-github)
4. [Updating from an ORT version to Another One](#updating-from-an-ort-version-to-another-one)



## Prerequisites
All changes in ORT have been (and should be!) labeled in the code with `WITH_UE` in some way (e.g., `#ifdef WITH_UE`, `// WITH_UE`, etc). Search for `WITH_UE` to (hopefully) find all the custom changes we made. When adding new changes to ORT, please, make sure to keep adding this `WITH_UE` flag to help locate changes in the future.



## Upgrading ONNX Runtime
### Step 0: Compiling Third Parties
Francisco Vicente understands this step better, in case of questions, ping him.
- MLAS (manually remove the old ORT_MLAS folder first, and repeate process for every platform):
```
################################################## PARAMETERS ##################################################
$ORT_MLAS_PARENT_PATH = "D:/Users/gines.hidalgo/Downloads/" # In Epic machine, otherwise: D:/Users/gineshidalgo99/Desktop/ONNXRuntime
$NNI_ORT_MLAS_LOCATION = "D:/P4/ue5_main_pitt64/Engine/Plugins/Experimental/NeuralNetworkInference/Source/ThirdParty/Deps"
$NNI_ORT_MLAS_FINAL_NAME = "MLAS_2021_10_20"
$NNI_ORT_MLAS_OLD_NAME = "MLAS_2021_10_19"
$FINAL_COMMIT_HASH = "4028e51e7e6421fdbeca5f4e4ccd8b4f790d0fd5" # The one NNI's ORT will be using after this merge

################################################## AUTOMATIC SCRIPT ##################################################
cd "$ORT_MLAS_PARENT_PATH"
mkdir ORT_MLAS; cd ORT_MLAS
git clone --recursive https://github.com/Microsoft/onnxruntime
cd onnxruntime
# Checkout ORT master. E.g., on Oct 18th
git reset --hard $FINAL_COMMIT_HASH
.\build.bat --config Release --parallel --use_dml --use_full_protobuf
```
Copy the new MLAS into `{NNI}/Source/ThirdParty/Deps/MLAS_YYYY_MM_DD/`:
```
################################################## AUTOMATIC SCRIPT ##################################################
$NNI_ORT_MLAS_FINAL_PATH = "$NNI_ORT_MLAS_LOCATION/$NNI_ORT_MLAS_FINAL_NAME"
$NNI_ORT_MLAS_OLD_PATH = "$NNI_ORT_MLAS_LOCATION/$NNI_ORT_MLAS_OLD_NAME"
mkdir $NNI_ORT_MLAS_FINAL_PATH
mkdir $NNI_ORT_MLAS_FINAL_PATH/include/core/mlas
mkdir $NNI_ORT_MLAS_FINAL_PATH/lib/Win64/
ni $NNI_ORT_MLAS_FINAL_PATH/$FINAL_COMMIT_HASH
cp -r -fo $ORT_MLAS_PARENT_PATH/ORT_MLAS/onnxruntime/onnxruntime/core/mlas/ $NNI_ORT_MLAS_FINAL_PATH/include/core/
cp -r -fo $ORT_MLAS_PARENT_PATH/ORT_MLAS/onnxruntime/build/Windows/Release/Release/onnxruntime_mlas.lib $NNI_ORT_MLAS_FINAL_PATH/lib/Win64/onnxruntime_mlas.lib
cp -r -fo $NNI_ORT_MLAS_OLD_PATH/MLAS_TPS_README.txt $NNI_ORT_MLAS_FINAL_PATH/MLAS_TPS_README.txt
cp -r -fo $NNI_ORT_MLAS_OLD_PATH/ONNXRuntime.tps $NNI_ORT_MLAS_FINAL_PATH/ONNXRuntime.tps
cp -r -fo $NNI_ORT_MLAS_OLD_PATH/ONNXRuntime${NNI_ORT_MLAS_OLD_NAME}.Build.cs $NNI_ORT_MLAS_FINAL_PATH/ONNXRuntime${NNI_ORT_MLAS_FINAL_NAME}.Build.cs
```

- ONNX: Compile desired ORT version to see ONNX flags + https://github.ol.epicgames.net/francisco-vicente/onnx_nni



### Step 1: Prerequisites
1. Create and open the following folders on your explorer:
	- A wrapper folder `$ORT_TEMP`, e.g., `D:/Users/gineshidalgo99/Desktop/ONNXRuntime`
	- `${$ORT_TEMP}/ONNXRuntime_code_from_NNI`
	- `${$ORT_TEMP}/onnxruntime`
	- `${$ORT_TEMP}/ONNXRuntime_code_to_push_to_NNI`
2. Fork https://github.com/Microsoft/onnxruntime into your GitHub account, e.g., https://github.com/gineshidalgo99/onnxruntime



### Step 2: Create Local ONNX Runtime Fork
(First time only, not needed if you already have your fork of ORT locally) Clone your fork of ORT locally:
```
################################################## PARAMETERS ##################################################
$ORT_PARENT_PATH = "D:/Users/gineshidalgo99/Desktop/ONNXRuntime" # D:/Users/gines.hidalgo/Desktop/ONNXRuntime

################################################## AUTOMATIC SCRIPT ##################################################
cd $ORT_PARENT_PATH
# Recursive not needed if only planning to merge (but not compile)
# git clone --recursive https://github.com/gineshidalgo99/onnxruntime # git clone --recursive https://github.com/Microsoft/onnxruntime
git clone https://github.com/gineshidalgo99/onnxruntime
cd onnxruntime
```



### Step 3: Add NNI's Changes Locally
Before running the commands below:
1. Copy NNI's ONNXRuntime locally:
	- From `{UE5}/Engine/Plugins/Experimental/NeuralNetworkInference/Source/ThirdParty/ONNXRuntime/`.
	- Into `${$ORT_TEMP}/ONNXRuntime_code_from_NNI/`.
2. To allow changes, right-click on `ONNXRuntime_code_from_NNI`, "Properties", uncheck "Read-only", and "OK".

You can now run the following commands (idea of what the commands below will automatically do):
	1. `ONNXRuntime/Internal/`:
		- Copy subset of `{onnxruntime_path}/include/onnxruntime/core/` into `ONNXRuntime/Internal/core/`.
	2. `ONNXRuntime/Private/`:
		- Copy subset of `{onnxruntime_path}/onnxruntime/contrib_ops/cpu/` into `ONNXRuntime/Private/contrib_ops/cpu/`.
		- Copy subset of `{onnxruntime_path}/onnxruntime/core/` into `ONNXRuntime/Private/core/`.
		- Copy subset of `{onnxruntime_path}/onnxruntime/test/testdata/custom_op_library/` into `ONNXRuntime/Private/test/testdata/custom_op_library/`.

With commands:
- Part 1: Reverting your ORT fork (locally) to the right commit:
```
################################################## PARAMETERS ##################################################
$ORT_PARENT_PATH = "D:/Users/gineshidalgo99/Desktop/ONNXRuntime" # D:/Users/gines.hidalgo/Desktop/ONNXRuntime
$CURRENT_NNI_ORT_COMMIT_HASH = "1aa21df149fe7be5ac39b3d23f80234a6f4d7890" # The one NNI's ORT is using on UE5/Main

################################################## AUTOMATIC SCRIPT ##################################################
########## RESETING TO CURRENT_NNI_ORT_COMMIT_HASH ##########
$ORT_PATH = "${ORT_PARENT_PATH}/onnxruntime"
$ORT_FROM_NNI = "${ORT_PARENT_PATH}/ONNXRuntime_code_from_NNI"
cd $ORT_PATH

git pull https://github.com/microsoft/onnxruntime/ master
git reset *; git checkout *; git clean -f -d; git reset --hard $CURRENT_NNI_ORT_COMMIT_HASH
# git push -f # Optional
git status
```
- Part 2: Adding the new code
```
########## REMOVING ##########
# Remove include/onnxruntime
rm -r -fo $ORT_PATH/include/onnxruntime/core
# Remove contrib_ops/cpu
rm -r -fo $ORT_PATH/onnxruntime/contrib_ops/cpu/
# Remove core
rm -r -fo $ORT_PATH/onnxruntime/core/common
rm -r -fo $ORT_PATH/onnxruntime/core/flatbuffers/
rm -r -fo $ORT_PATH/onnxruntime/core/framework/
rm -r -fo $ORT_PATH/onnxruntime/core/graph/
rm -r -fo $ORT_PATH/onnxruntime/core/optimizer/
rm -r -fo $ORT_PATH/onnxruntime/core/platform/
rm -r -fo $ORT_PATH/onnxruntime/core/profile/
rm -r -fo $ORT_PATH/onnxruntime/core/providers/cpu/
rm -r -fo $ORT_PATH/onnxruntime/core/providers/dml/
rm -r -fo $ORT_PATH/onnxruntime/core/providers/shared/
rm -r -fo $ORT_PATH/onnxruntime/core/providers/shared_library/
git checkout onnxruntime/core/providers/shared_library/provider_bridge_provider.cc # This file was removed from NNI because it is not used and causes issues
rm -r -fo $ORT_PATH/onnxruntime/core/providers/common.h
rm -r -fo $ORT_PATH/onnxruntime/core/providers/get_execution_providers.*
rm -r -fo $ORT_PATH/onnxruntime/core/providers/op_kernel_type_control*
rm -r -fo $ORT_PATH/onnxruntime/core/quantization/
rm -r -fo $ORT_PATH/onnxruntime/core/session/
rm -r -fo $ORT_PATH/onnxruntime/core/util/
# Remove custom_op_library
rm -r -fo $ORT_PATH/onnxruntime/test/testdata/custom_op_library/custom_op_library.cc
rm -r -fo $ORT_PATH/onnxruntime/test/testdata/custom_op_library/custom_op_library.h

################################################## ADDING NEW FILES ##################################################
# Copy include/onnxruntime
cp -r -fo $ORT_FROM_NNI/Internal/core $ORT_PATH/include/onnxruntime/core
# Copy contrib_ops/cpu
cp -r -fo $ORT_FROM_NNI/Private/contrib_ops/cpu $ORT_PATH/onnxruntime/contrib_ops
# Copy core
cp -r -fo $ORT_FROM_NNI/Private/core $ORT_PATH/onnxruntime
cp -r -fo $ORT_FROM_NNI/Private_DML/Windows/core/ $ORT_PATH/onnxruntime
# Copy custom_op_library
cp -r -fo $ORT_FROM_NNI/Private/test $ORT_PATH/onnxruntime

################################################## REVERTING ACCIDENTAL DELETES ##################################################
git checkout onnxruntime/core/platform/android/*
git checkout onnxruntime/core/platform/posix/env*
git checkout onnxruntime/core/platform/posix/logging/*
git checkout onnxruntime/core/platform/windows/*

# Option a
# git add .
# git status

# Option b
git add include/onnxruntime/core/session/onnxruntime_cxx_inline.*
git add onnxruntime/contrib_ops/cpu/*activations.cc
git add onnxruntime/contrib_ops/cpu/*element_wise_ops.cc
git add onnxruntime/contrib_ops/cpu/*unique.cc
git add onnxruntime/core/framework/*framework_utils.cc
git add onnxruntime/core/framework/*utils.cc
git add onnxruntime/core/providers/cpu/controlflow/*utils.cc
git add onnxruntime/core/optimizer/ort_format_runtime_optimization/*utils.cc
git add onnxruntime/core/optimizer/*utils.cc
git status
git diff --cached
```

See how many files you have updated and make sure ALL changes reported by `git diff` come from lines saying `WITH_UE` (otherwise you might have messed up and merged the wrong version of ORT, believe me, I've done that mistake before, and it's a nightmare, so CHECK IT!):
```
cd $ORT_PATH
# Using git add will let you see renames more easily, just don't commit/push it!
git add .
git status
git diff --cached
# Undo git add
git reset *
```

(OPTIONAL) Reset the local changes
```
cd $ORT_PATH
# Note: To remove untracked files
git reset *
git checkout *
git clean -f -d   # https://koukia.ca/how-to-remove-local-untracked-files-from-the-current-git-branch-571c6ce9b6b1
```



### Step 4: Merge ORT Master with NNI's Canges and Create Zip to Copy to NNI
```
################################################## PARAMETERS ##################################################
$FINAL_COMMIT_HASH = "1aa21df149fe7be5ac39b3d23f80234a6f4d7890" # The one NNI's ORT will be using after this merge
$ORT_PARENT_PATH = "D:/Users/gineshidalgo99/Desktop/ONNXRuntime" # "D:/Users/gines.hidalgo/Desktop/ONNXRuntime"
$FINAL_ZIP_FILE_PATH = "${ORT_PARENT_PATH}/ort_compressed.zip"
```

Pull and merge with Microsoft::ONNXRuntime master:
```
################################################## AUTOMATIC SCRIPT ##################################################
# Push code
git add .
git commit -m "NNI"

# Checkout desired version to merged with (e.g., ORT master on Oct 18th)
git pull https://github.com/microsoft/onnxruntime/ $FINAL_COMMIT_HASH
# NOTE: Manually fix conflicts locally (if any). E.g., if error(s) about untracked files, just run something like this with whatever files you get an error message about:
# rm onnxruntime/python/tools/tensorrt/perf/build/Dockerfile.tensorrt-perf
```

(OPTIONAL and not needed at all) If no conflicts or after solving them, code ready to be moved to NNI. You can optionally commmit/push it:
```
git add .
git commit -m "Master from MONTH DAY-th merged"
git push
```

You can now create the zip file to Copy to NNI:
```
################################################## AUTOMATIC SCRIPT ##################################################
$ORT_PATH = "${ORT_PARENT_PATH}/onnxruntime"
$ORT_FROM_NNI = "${ORT_PARENT_PATH}/ONNXRuntime_code_from_NNI"
$ORT_CODE_TO_PUSH_TO_NNI = "${ORT_PARENT_PATH}/ONNXRuntime_code_to_push_to_NNI"
cd $ORT_CODE_TO_PUSH_TO_NNI

rm ${ORT_CODE_TO_PUSH_TO_NNI}/* # It will ask for confirmation, press "A" (Yes to All)

# Copy include/onnxruntime
cp -r -fo ${ORT_PATH}/include/onnxruntime/core ${ORT_CODE_TO_PUSH_TO_NNI}/Internal/core
cp -r -fo $ORT_FROM_NNI/Internal/onnxruntime_config.h ${ORT_CODE_TO_PUSH_TO_NNI}/Internal/
# Copy ORTModule.h/cpp
mkdir ${ORT_CODE_TO_PUSH_TO_NNI}/Private/
cp -r -fo $ORT_FROM_NNI/Private/Module.* ${ORT_CODE_TO_PUSH_TO_NNI}/Private/
# Copy contrib_ops/cpu
cp -r -fo ${ORT_PATH}/onnxruntime/contrib_ops/cpu ${ORT_CODE_TO_PUSH_TO_NNI}/Private/contrib_ops/cpu
# Copy core
cp -r -fo ${ORT_PATH}/onnxruntime/core/common ${ORT_CODE_TO_PUSH_TO_NNI}/Private/core/common
cp -r -fo ${ORT_PATH}/onnxruntime/core/flatbuffers ${ORT_CODE_TO_PUSH_TO_NNI}/Private/core/flatbuffers
cp -r -fo ${ORT_PATH}/onnxruntime/core/framework ${ORT_CODE_TO_PUSH_TO_NNI}/Private/core/framework
cp -r -fo ${ORT_PATH}/onnxruntime/core/graph ${ORT_CODE_TO_PUSH_TO_NNI}/Private/core/graph
cp -r -fo ${ORT_PATH}/onnxruntime/core/optimizer ${ORT_CODE_TO_PUSH_TO_NNI}/Private/core/optimizer
cp -r -fo ${ORT_PATH}/onnxruntime/core/platform ${ORT_CODE_TO_PUSH_TO_NNI}/Private/core/
rm -r -fo ${ORT_CODE_TO_PUSH_TO_NNI}/Private/core/platform/android
rm -r -fo ${ORT_CODE_TO_PUSH_TO_NNI}/Private/core/platform/posix/env*
rm -r -fo ${ORT_CODE_TO_PUSH_TO_NNI}/Private/core/platform/posix/logging
rm -r -fo ${ORT_CODE_TO_PUSH_TO_NNI}/Private/core/platform/windows
cp -r -fo ${ORT_PATH}/onnxruntime/core/profile ${ORT_CODE_TO_PUSH_TO_NNI}/Private/core/profile
cp -r -fo ${ORT_PATH}/onnxruntime/core/providers/cpu ${ORT_CODE_TO_PUSH_TO_NNI}/Private/core/providers/cpu
cp -r -fo ${ORT_PATH}/onnxruntime/core/providers/dml ${ORT_CODE_TO_PUSH_TO_NNI}/Private_DML/Windows/core/providers/dml
cp -r -fo ${ORT_PATH}/onnxruntime/core/providers/shared ${ORT_CODE_TO_PUSH_TO_NNI}/Private/core/providers/shared
cp -r -fo ${ORT_PATH}/onnxruntime/core/providers/shared_library ${ORT_CODE_TO_PUSH_TO_NNI}/Private/core/providers/shared_library
rm ${ORT_CODE_TO_PUSH_TO_NNI}/Private/core/providers/shared_library/provider_bridge_provider.cc
cp -r -fo ${ORT_PATH}/onnxruntime/core/providers/*.cc ${ORT_CODE_TO_PUSH_TO_NNI}/Private/core/providers/
cp -r -fo ${ORT_PATH}/onnxruntime/core/providers/*.h ${ORT_CODE_TO_PUSH_TO_NNI}/Private/core/providers/
cp -r -fo ${ORT_PATH}/onnxruntime/core/providers/*.inc ${ORT_CODE_TO_PUSH_TO_NNI}/Private/core/providers/
mv -fo ${ORT_CODE_TO_PUSH_TO_NNI}/Private/core/providers/utils.cc ${ORT_CODE_TO_PUSH_TO_NNI}/Private/core/providers/core_prov_utils.cc
cp -r -fo ${ORT_PATH}/onnxruntime/core/quantization ${ORT_CODE_TO_PUSH_TO_NNI}/Private/core/quantization
cp -r -fo ${ORT_PATH}/onnxruntime/core/session ${ORT_CODE_TO_PUSH_TO_NNI}/Private/core/session
cp -r -fo ${ORT_PATH}/onnxruntime/core/util ${ORT_CODE_TO_PUSH_TO_NNI}/Private/core/util
# Copy custom_op_library
mkdir ${ORT_CODE_TO_PUSH_TO_NNI}/Private/test/testdata/custom_op_library
cp -r -fo ${ORT_PATH}/onnxruntime/test/testdata/custom_op_library/custom_op_library.cc ${ORT_CODE_TO_PUSH_TO_NNI}/Private/test/testdata/custom_op_library/custom_op_library.cc
cp -r -fo ${ORT_PATH}/onnxruntime/test/testdata/custom_op_library/custom_op_library.h ${ORT_CODE_TO_PUSH_TO_NNI}/Private/test/testdata/custom_op_library/custom_op_library.h

# Individual files
ni ${ORT_CODE_TO_PUSH_TO_NNI}/$FINAL_COMMIT_HASH
cp $ORT_FROM_NNI/ONNXRuntime.Build.cs ${ORT_CODE_TO_PUSH_TO_NNI}/ONNXRuntime.Build.cs
cp $ORT_FROM_NNI/ONNXRuntime.tps ${ORT_CODE_TO_PUSH_TO_NNI}/ONNXRuntime.tps
cp $ORT_FROM_NNI/Readme_how_to_update_ORT_version.md ${ORT_CODE_TO_PUSH_TO_NNI}/Readme_how_to_update_ORT_version.md

rm $FINAL_ZIP_FILE_PATH
Compress-Archive -LiteralPath ${ORT_CODE_TO_PUSH_TO_NNI} -DestinationPath $FINAL_ZIP_FILE_PATH
```

This new code zipped as `${$ORT_TEMP}/ort_compressed.zip` can be copied into NNI and tested in there. To test it properly, do the following tests (in this order):
1. Compile UE and run QA tests to make sure they are successful.
2. Package game for Windows and run tests on the Windows game to make sure they are successful. Trick: If your game is saved on `D:/Users/gines.hidalgo/Desktop/GameTest/`, the logging of the application will be saved on `D:/Users/gines.hidalgo/Desktop/GameTest/Windows/NNIExample/Saved/Logs/NNIExample.log`.
3. Package game for Linux from Windows to make sure it compiles on Linux.
4. Run the Static Analysis. How to run "UE4 Static Analysis Win64 (MSVC)" locally:
	1. Sync the latest green commit to minimize non-NNI errors/warnings.
	2. Add `PS5`, `Stadia`, `WinGDK`, and `XboxOneGDK/XSX` to the UE filter for this branch (`UGS` -> `Options` -> `uSync Filter...` -> `Current Workspace`)
	3. Set the default value of the following to false:
		- `WithFortniteGame`
		- `WithFortniteClient`
		- `WithFNFastTest`
		- `WithFNTest`
	4. Run from PowerShell:
		```
		cd D:/P4/ue5_main_pitt64_3/
		Engine\Build\BatchFiles\RunUAT.bat BuildGraph -Script="Engine/Restricted/NotForLicensees/Build/DevStreams.xml" -Target="UE4 Static Analysis Win64 (MSVC)" -P4
		cd D:/P4/ue5_main_pitt64_3/Engine/Build/BatchFiles
		./RunUAT.bat BuildGraph -Script="Engine/Restricted/NotForLicensees/Build/DevStreams.xml" -Target="UE4 Static Analysis Win64 (MSVC)" -P4
		```
		- Note: If weird errors, you could add `-verbose` at the end of the command.
		- PVS command (requires license):
			```
			cd D:/P4/ue5_main_pitt64_3/
			Engine\Build\BatchFiles\RunUAT.bat BuildGraph -Script="Engine/Restricted/NotForLicensees/Build/DevStreams.xml" -Target="UE4 Static Analysis Win64 (PVS-Studio)" -SkipTargetsWithoutTokens *> pvs_test.txt
			# See log from pvs_test.txt
			# Redirect-to-file info: https://docs.microsoft.com/en-us/powershell/module/microsoft.powershell.core/about/about_redirection
			```
	5. Optionally compile with Clang (on Windows). Very important, use https://github.com/llvm/llvm-project/releases/tag/llvmorg-12.0.1, 13 will not work. Sync `Sandbox/DevTools/Clang/Clang.uproject` from P4 and run it from UGS. Or alternatively by:
	```
	# https://wiki.it.epicgames.net/display/UEWiki/Epic%27s+Build+System
	cd D:/P4/ue5_main_pitt64_3/
	Engine\Build\BatchFiles\RunUAT.bat BuildGraph -Script="Engine/Restricted/NotForLicensees/Build/DevStreams.xml" -Target="Compile ClangEditor Win64" -P4 -SkipTargetsWithoutTokens
	```
	6. Log generated in: `Engine/Programs/AutomationTool/Saved/Logs/Log.txt`
	7. Optionally run NonUnity Compile UnrealEditor Win64:
	```
	cd D:/P4/ue5_main_pitt64_2/
	Engine\Build\BatchFiles\RunUAT.bat BuildGraph -Script="Engine/Restricted/NotForLicensees/Build/DevStreams.xml" -Target="NonUnity Compile UnrealEditor Win64" -P4 -SkipTargetsWithoutTokens *> D:/Users/gines.hidalgo/Desktop/NonUnityTest.txt
	```
5. Do extensive pre-flights for all targeted platforms by running a `Editor, Tools & Monolithics` and a `Full Build` test.

Once fully working on NNI, pushed code on P4 and you are done!



### Optional Step: Making Pull Request to ORT
```
git checkout -b SOME_BRANCH_NAME # Eg ClangOnWindowsCompiling, AlwaysReturn
git push origin SOME_BRANCH_NAME
# Equivalent to:
# git branch SOME_BRANCH_NAME
# git checkout SOME_BRANCH_NAME
```



### Final Architecture of ONNXRuntime in NNI
- `NNI/Source/ThirdParty/`
	- `ONNXRuntime/`
		- `Internal/`
			- `core/`
				- `common/`
				- `framework/`
				- `graph/`
				- `optimizer/`
				- `platform/`
				- `providers/`
				- `session/`
			- `onnxruntime_config.h`
		- `Private/`
			- `contrib_ops/cpu/`
			- `core/`
				- `common/`
				- `contrib_ops/`
				- `custom_ops/`
				- `flatbuffers/`
				- `framework/`
				- `graph/`
				- `optimizer/`
				- `platform/`
					- `UE/`
				- `profile/`
				- `providers/` (removed non-used providers, dml one moved into `Private_DML/`)
					- `cpu/`
					- `nni_cpu/` (eventually)
					- `nni_hlsl/` (eventually)
					- `shared/`
					- `shared_library/`
				- `quantization/`
				- `session/`
				- `util/`
			- `test/testdata/custom_op_library/`
		- `Private_DML/`
			- `Windows/`
				- `core/`
					- `providers/`
						- `dml/`



### FAQ
If minor compiler error on NNI about version missmatch:
- Bump version in `[...]/ONNXRuntime_code_from_NNI/Internal/onnxruntime_config.h` accordingly: `#define ORT_VERSION "1.9.0"` (otherwise minor compiler error on NNI).
	- Additional info: In the original ORT, `onnxruntime_config.h` is created by `{onnxruntime_path}/cmake/CMakeLists.txt` in line 1151.
		- `configure_file(onnxruntime_config.h.in ${CMAKE_CURRENT_BINARY_DIR}/onnxruntime_config.h)`



## Test and Debug ONNX Runtime on GitHub
- ORT CI (Test/Fix some PR)
```
git clone https://github.com/gineshidalgo99/onnxruntime
git checkout REPO_NAME
# What ORT guys run on their CI system
\build.bat --config Release --build_dir build --build_shared_lib --cmake_generator "Visual Studio 16 2019" --build_wheel --use_winml --build_shared_lib --enable_wcos --use_dnnl --use_dml --parallel
# python.exe tools\ci_build\build.py --config RelWithDebInfo --build_dir build --build_shared_lib --cmake_generator "Visual Studio 16 2019" --build_wheel --use_winml --build_shared_lib --enable_wcos --use_dnnl
# git submodule update --init --recursive
# python.exe tools\ci_build\build.py --config RelWithDebInfo --build_dir b --skip_submodule_sync --build_shared_lib --cmake_generator "Visual Studio 16 2019" --build_wheel --use_winml --build_shared_lib --enable_wcos --use_dnnl --build_java --build_nodejs
```

- Paco's 1.9.1
```
.\build.bat --config Release --parallel --use_dml --use_full_protobuf
```

- Me 1.7.1
```
.\build.bat --config Release --use_dml --build_shared_lib --parallel --cmake_generator "Visual Studio 16 2019" --build_wheel
```

- Paco's 1.7.1
```
.\build.bat --config Release --use_dml --build_shared_lib --parallel --skip_tests --cmake_generator "Visual Studio 16 2019" --build_wheel
```



## Updating from an ORT version to Another One
Updating from master commit `X` to master commit `Y` is relatively "easy" and is detailed below. However, updating from a specific version or branch of ONNX Runtime into another version/master/branch will be trickier because the changes from that branch will have to be reverted first. For this special case, these would be the steps:
1. Similarly to the documentation above, downgrade from your current version (e.g. 1.7.1) into the last common commit between that version and master (e.g. the last shared commit between 1.7.1 and master, which would be before v1.7.1 was created).
2. Fix merge conflicts (caused by ONNX Runtime, not by NNI).
3. Update from this master commit to the desired master commit by following the document below.