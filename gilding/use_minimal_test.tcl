# 在已打开 project_1.xpr 的 Vivado Tcl Console 中执行：
# source use_minimal_test.tcl

set script_dir [file dirname [file normalize [info script]]]
set min_v   [file join $script_dir project_1.srcs sources_1 new ad9744_minimal_test.v]
set min_xdc [file join $script_dir project_1.srcs constrs_1 new ad9744_minimal_test.xdc]

# 确保共享 Clock Wizard 已按当前方案重新生成为 150 MHz，而不是继续使用
# Vivado 缓存中的旧 180 MHz 输出产品。
source [file join $script_dir create_clock_ip.tcl]

if {[llength [get_files -quiet $min_v]] == 0} {
    add_files -fileset sources_1 -norecurse $min_v
}
if {[llength [get_files -quiet $min_xdc]] == 0} {
    add_files -fileset constrs_1 -norecurse $min_xdc
}

set_property top ad9744_minimal_test [get_filesets sources_1]

foreach old_xdc [get_files -quiet {ad9744_lcd_adapter.xdc ad9744_ch2.xdc}] {
    set_property used_in_synthesis false $old_xdc
    set_property used_in_implementation false $old_xdc
}
set min_xdc_obj [get_files $min_xdc]
set_property used_in_synthesis true $min_xdc_obj
set_property used_in_implementation true $min_xdc_obj

update_compile_order -fileset sources_1
reset_run synth_1
puts "已切换到 ad9744_minimal_test；请重新运行综合、实现并生成 bitstream。"
