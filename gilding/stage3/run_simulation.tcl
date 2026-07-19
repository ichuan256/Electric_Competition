set script_dir [file dirname [file normalize [info script]]]
open_project [file join $script_dir stage3.xpr]
set_property top tb_thd_harmonic_reconstruction [get_filesets sim_1]
update_compile_order -fileset sim_1
launch_simulation -mode behavioral
run all
close_sim
set_property top tb_thd_uart_v2 [get_filesets sim_1]
update_compile_order -fileset sim_1
launch_simulation -mode behavioral
run all
close_sim
close_project
