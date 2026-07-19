set script_dir [file dirname [file normalize [info script]]]
create_project -force stage3_uart_sim G:/fpga/tmp/stage3_uart_sim -part xc7z020clg400-2
add_files -norecurse [file join $script_dir rtl thd_uart_v2.v]
add_files -fileset sim_1 -norecurse [file join $script_dir sim tb_thd_uart_v2.v]
set_property top tb_thd_uart_v2 [get_filesets sim_1]
launch_simulation -mode behavioral
run all
close_sim
close_project
