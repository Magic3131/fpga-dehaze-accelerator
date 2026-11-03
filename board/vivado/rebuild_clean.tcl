# ============================================================
#  rebuild_clean.tcl —— 异地"从头重新构建"验证脚本
#
#  目的：证明工程换一台电脑 / 换一个路径也能重新生成比特流。
#  关键点：只使用工程自带的 ip_repo/（已内含 Digilent 的
#          dvi2rgb / rgb2dvi / axi_dynclk），不依赖 D:/AMD/vivado-library。
#
#  用法（在工程目录下）：
#      vivado -mode batch -source rebuild_clean.tcl
#  产物：
#      vivado_rebuild.log（由外层重定向产生）
#      utilization.rpt
#      timing_summary.rpt
#      test.runs/impl_1/dehaze_top.bit
# ============================================================

set proj_dir [file normalize [file dirname [info script]]]
set xpr      "$proj_dir/test.xpr"

puts "============================================================"
puts "  CLEAN REBUILD"
puts "  project dir : $proj_dir"
puts "============================================================"

if {![file exists $xpr]} {
    puts "!! FATAL: $xpr not found"
    exit 1
}
open_project $xpr
puts "=== \[1\] project opened"

# ---------- 只使用工程自带的 IP 仓库 ----------
set local_repo "$proj_dir/ip_repo"
if {![file exists $local_repo]} {
    puts "!! FATAL: ip_repo not found at $local_repo"
    exit 1
}
set_property ip_repo_paths [list $local_repo] [current_project]
update_ip_catalog -rebuild
puts "=== \[2\] ip_repo_paths = [get_property ip_repo_paths [current_project]]"
foreach p {dvi2rgb rgb2dvi axi_dynclk dehaze} {
    set d [get_ipdefs -quiet -all -filter "NAME == $p"]
    puts "        $p : $d"
}

# ---------- 从头生成 Block Design 输出产物 ----------
set bds [get_files -quiet *.bd]
if {[llength $bds] == 0} {
    puts "!! FATAL: no block design found"
    exit 1
}
puts "=== \[3\] block design(s): $bds"
foreach bd $bds {
    puts "        regenerating [file tail $bd]"
    catch {reset_target all $bd}
    if {[catch {generate_target -force all $bd} e]} {
        puts "!! FATAL: generate_target failed: $e"
        exit 1
    }
}
puts "=== \[3\] block design regenerated"

# ---------- 综合 ----------
puts "=== \[4\] synth_1 start"
catch {reset_run synth_1}
launch_runs synth_1 -jobs 8
wait_on_run synth_1
set sp [get_property PROGRESS [get_runs synth_1]]
puts "=== \[4\] synth_1 progress = $sp  status = [get_property STATUS [get_runs synth_1]]"
if {$sp ne "100%"} {
    puts "!! FATAL: synthesis did not finish"
    exit 1
}

# ---------- 实现 + 写比特流 ----------
puts "=== \[5\] impl_1 start"
catch {reset_run impl_1}
launch_runs impl_1 -to_step write_bitstream -jobs 8
wait_on_run impl_1
set ip [get_property PROGRESS [get_runs impl_1]]
puts "=== \[5\] impl_1 progress = $ip  status = [get_property STATUS [get_runs impl_1]]"
if {$ip ne "100%"} {
    puts "!! FATAL: implementation did not finish"
    exit 1
}

# ---------- 导出队友要的两份报告 ----------
puts "=== \[6\] writing reports"
open_run impl_1
if {[catch {report_utilization -file "$proj_dir/utilization.rpt"} e1]} {
    puts "!! report_utilization failed: $e1"
}
if {[catch {report_timing_summary -file "$proj_dir/timing_summary.rpt"} e2]} {
    puts "!! report_timing_summary failed: $e2"
}

# ---------- 结果汇总 ----------
set clks [get_clocks -quiet *]
puts "=== \[7\] clocks: $clks"
set bits [glob -nocomplain "$proj_dir/test.runs/impl_1/*.bit"]
foreach b $bits {
    puts "=== \[7\] bitstream: $b  ([file size $b] bytes)"
}
foreach r {utilization.rpt timing_summary.rpt} {
    set f "$proj_dir/$r"
    if {[file exists $f]} { puts "=== \[7\] report: $f  ([file size $f] bytes)" }
}

close_project
puts "=== CLEAN REBUILD DONE"
exit 0
