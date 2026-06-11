################################################################################
# Automatically-generated file. Do not edit!
################################################################################

SHELL = cmd.exe

# Each subdirectory must supply rules for building sources it contributes
device/%.obj: ../device/%.c $(GEN_OPTS) | $(GEN_FILES) $(GEN_MISC_FILES)
	@echo 'Building file: "$<"'
	@echo 'Invoking: C2000 Compiler'
	"C:/ti/ccs2041/ccs/tools/compiler/ti-cgt-c2000_22.6.3.LTS/bin/cl2000" -v28 -ml -mt --cla_support=cla2 --float_support=fpu32 --tmu_support=tmu0 --vcu_support=vcu0 -O3 --opt_for_speed=5 --fp_mode=relaxed --include_path="C:/Users/Intern 1/workspace_ccstheia/totempole_spll" --include_path="C:/ti/C2000Ware_26_00_00_00" --include_path="C:/Users/Intern 1/workspace_ccstheia/totempole_spll/device" --include_path="C:/ti/C2000Ware_26_00_00_00/driverlib/f28004x/driverlib/" --include_path="C:/ti/ccs2041/ccs/tools/compiler/ti-cgt-c2000_22.6.3.LTS/include" --include_path="C:/Users/Intern 1/workspace_ccstheia/totempole_spll/libraries/spll" --define=DEBUG --define=_LAUNCHXL_F280049C --define=_FLASH --diag_suppress=10063 --diag_warning=225 --diag_wrap=off --display_error_number --gen_func_subsections=on --abi=eabi --preproc_with_compile --preproc_dependency="device/$(basename $(<F)).d_raw" --include_path="C:/Users/Intern 1/workspace_ccstheia/totempole_spll/CPU1_LAUNCHXL_FLASH/syscfg" --obj_directory="device" $(GEN_OPTS__FLAG) "$<"
	@echo 'Finished building: "$<"'
	@echo ' '

device/%.obj: ../device/%.asm $(GEN_OPTS) | $(GEN_FILES) $(GEN_MISC_FILES)
	@echo 'Building file: "$<"'
	@echo 'Invoking: C2000 Compiler'
	"C:/ti/ccs2041/ccs/tools/compiler/ti-cgt-c2000_22.6.3.LTS/bin/cl2000" -v28 -ml -mt --cla_support=cla2 --float_support=fpu32 --tmu_support=tmu0 --vcu_support=vcu0 -O3 --opt_for_speed=5 --fp_mode=relaxed --include_path="C:/Users/Intern 1/workspace_ccstheia/totempole_spll" --include_path="C:/ti/C2000Ware_26_00_00_00" --include_path="C:/Users/Intern 1/workspace_ccstheia/totempole_spll/device" --include_path="C:/ti/C2000Ware_26_00_00_00/driverlib/f28004x/driverlib/" --include_path="C:/ti/ccs2041/ccs/tools/compiler/ti-cgt-c2000_22.6.3.LTS/include" --include_path="C:/Users/Intern 1/workspace_ccstheia/totempole_spll/libraries/spll" --define=DEBUG --define=_LAUNCHXL_F280049C --define=_FLASH --diag_suppress=10063 --diag_warning=225 --diag_wrap=off --display_error_number --gen_func_subsections=on --abi=eabi --preproc_with_compile --preproc_dependency="device/$(basename $(<F)).d_raw" --include_path="C:/Users/Intern 1/workspace_ccstheia/totempole_spll/CPU1_LAUNCHXL_FLASH/syscfg" --obj_directory="device" $(GEN_OPTS__FLAG) "$<"
	@echo 'Finished building: "$<"'
	@echo ' '


