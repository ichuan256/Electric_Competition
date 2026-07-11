# 在 project_1.xpr 已打开的 Vivado Tcl Console 中执行：
# source add_dual_dac_sources.tcl
# 用途：把第二路 DAC Verilog/XDC 加入当前工程会话并更新编译顺序。

set script_dir [file dirname [file normalize [info script]]]
set ch2_verilog [file join $script_dir project_1.srcs sources_1 new ad9744_dds_ch2.v]
set ch2_xdc     [file join $script_dir project_1.srcs constrs_1 new ad9744_ch2.xdc]

if {[llength [get_projects -quiet]] == 0} {
    open_project [file join $script_dir project_1.xpr]
}

if {[llength [get_files -quiet $ch2_verilog]] == 0} {
    add_files -fileset sources_1 -norecurse $ch2_verilog
}
if {[llength [get_files -quiet $ch2_xdc]] == 0} {
    add_files -fileset constrs_1 -norecurse $ch2_xdc
}

set_property USED_IN_SYNTHESIS true [get_files $ch2_verilog]
set_property USED_IN_IMPLEMENTATION true [get_files $ch2_verilog]
set_property USED_IN_SIMULATION true [get_files $ch2_verilog]
set_property USED_IN_SYNTHESIS true [get_files $ch2_xdc]
set_property USED_IN_IMPLEMENTATION true [get_files $ch2_xdc]
set_property top ad9744_dds_top [get_filesets sources_1]
update_compile_order -fileset sources_1
update_compile_order -fileset sim_1

puts "双 DAC 源文件已加入："
puts "  $ch2_verilog"
puts "  $ch2_xdc"
puts "请执行 reset_run synth_1 后重新综合。"
