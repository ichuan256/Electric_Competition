# Run from Vivado Tcl Console:
#   source G:/fpga/stage2/-/Gilding/stage3/rebuild_stage3.tcl
# Or in batch mode:
#   vivado -mode batch -source rebuild_stage3.tcl

set script_dir [file dirname [file normalize [info script]]]
set project_file [file join $script_dir stage3.xpr]

if {[llength [get_projects -quiet]] != 0} {
    close_project
}
create_project -force stage3 $script_dir -part xc7z020clg400-2

add_files -norecurse [list \
    [file join $script_dir rtl thd_recon_top.v] \
    [file join $script_dir rtl thd_harmonic_reconstruction.v] \
    [file join $script_dir rtl thd_uart_v2.v] \
    [file join $script_dir rtl sine_lut_4096.mem]]
set_property file_type {Memory Initialization Files} \
    [get_files [file join $script_dir rtl sine_lut_4096.mem]]
add_files -fileset constrs_1 -norecurse \
    [file join $script_dir constraints thd_recon_top.xdc]
add_files -fileset sim_1 -norecurse [list \
    [file join $script_dir sim tb_thd_harmonic_reconstruction.v] \
    [file join $script_dir sim tb_thd_uart_v2.v]]

set_property top thd_recon_top [current_fileset]
set_property top tb_thd_harmonic_reconstruction [get_filesets sim_1]

create_ip -name clk_wiz -vendor xilinx.com -library ip -module_name clk_wiz_0
set_property -dict [list \
    CONFIG.PRIM_IN_FREQ {50.000} \
    CONFIG.PRIM_SOURCE {No_buffer} \
    CONFIG.NUM_OUT_CLKS {1} \
    CONFIG.CLKOUT1_REQUESTED_OUT_FREQ {100.000} \
    CONFIG.USE_RESET {true} \
    CONFIG.RESET_TYPE {ACTIVE_HIGH} \
    CONFIG.RESET_PORT {reset} \
    CONFIG.USE_LOCKED {true}] [get_ips clk_wiz_0]
generate_target all [get_ips clk_wiz_0]
export_ip_user_files -of_objects [get_ips clk_wiz_0] \
    -no_script -sync -force -quiet

update_compile_order -fileset sources_1
update_compile_order -fileset sim_1
set_property STEPS.WRITE_BITSTREAM.ARGS.BIN_FILE true [get_runs impl_1]
# Vivado 2025.2 persists project changes immediately. `save_project` is parsed
# as `save_project_as` in this release and incorrectly asks for a project name.
close_project
puts "Stage3 project rebuilt: $project_file"
