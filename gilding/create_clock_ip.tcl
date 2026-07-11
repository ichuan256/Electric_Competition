# 在打开 project_1.xpr 后通过 Vivado Tcl Console 执行，也可以使用批处理命令：
# vivado -mode batch -source create_clock_ip.tcl
set script_dir [file dirname [file normalize [info script]]]
set project_file [file join $script_dir project_1.xpr]
if {[llength [get_projects -quiet]] == 0} { open_project $project_file }
if {[llength [get_ips -quiet clk_wiz_0]] == 0} {
    create_ip -name clk_wiz -vendor xilinx.com -library ip -module_name clk_wiz_0
}
set_property -dict [list \
    CONFIG.PRIM_IN_FREQ {50.000} \
    CONFIG.CLKOUT1_REQUESTED_OUT_FREQ {100.000} \
    CONFIG.USE_RESET {true} \
    CONFIG.RESET_TYPE {ACTIVE_LOW} \
    CONFIG.RESET_PORT {resetn} \
    CONFIG.USE_LOCKED {true}] [get_ips clk_wiz_0]
generate_target all [get_ips clk_wiz_0]
export_ip_user_files -of_objects [get_ips clk_wiz_0] -no_script -sync -force -quiet
update_compile_order -fileset sources_1
save_project
