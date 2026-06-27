/*
 * Copyright (C) 2026 by Thun Lu. All rights reserved.
 * Author: Thun Lu <thun.lu@zohomail.cn>
 * Repo:   https://github.com/thun-res/vlink
 *  _    __   __      _           __
 * | |  / /  / /     (_) ____    / /__
 * | | / /  / /     / / / __ \  / //_/
 * | |/ /  / /___  / / / / / / / ,<
 * |___/  /_____/ /_/ /_/ /_/ /_/|_|
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#pragma once

#include <string>

namespace vlink::bench::report::i18n {

inline std::string render_i18n_script_json_zh_cn() {
  return reinterpret_cast<const char*>(
      u8R"JSON({
    "report_heading_run": "性能测试",
    "report_heading_report": "报告",

    "skip_to_content": "跳到正文",
    "aria_main": "报告正文",

    "headline_subtitle": "覆盖本次场景矩阵的端到端传输、延迟、吞吐与序列化基准测试报告。",
    "summary_strip_label": "概要",
    "summary_pill_pass": "通过",
    "summary_pill_warn": "警告",
    "summary_pill_fail": "失败",
    "summary_pill_skipped": "跳过",
    "summary_pill_planned": "计划",
    "summary_pill_samples": "采样",



    "footer_generated": "由",
    "footer_format": "单文件离线 HTML 报告",
    "footer_doc": "方法说明请阅读 doc/10-cli-tools.md",

    "nav_recommendation": "测试对象推荐",
    "nav_overview": "本次概览",
    "nav_health": "传输健康",
    "nav_heatmap": "延迟/吞吐对比",
    "nav_suite_score": "分项结果",
    "nav_planner_notes": "运行备注",
    "nav_transport_rollup": "传输汇总",
    "nav_serialization": "序列化概览",
    "nav_charts": "趋势图",
    "nav_scenario_tables": "详细数据",

    "overview_title": "本次概览",
    "overview_samples": "采样数",
    "overview_planned": "计划用例",
    "overview_skipped": "跳过用例",
    "overview_cases": "用例数",
    "overview_passing": "通过",
    "overview_warning": "警告",
    "overview_failing": "失败",

    "health_badge_failures": "有失败",
    "health_badge_warnings": "警告",
    "health_badge_ok": "全部正常",
    "health_url_coverage_h3": "被测 URL 覆盖",
    "health_unstable_h3": "需要关注的用例",
    "health_note": "传输健康用来判断这份报告是否可信，再决定要不要比较分数。它会汇总失败、警告、URL 覆盖和异常用例：绿色表示没有明显异常；黄色表示需要复查丢包、跳过或不稳定结果；红色表示已有失败用例，应先看错误信息。少量丢包仍会记录；这里只突出可能影响结论的丢包、失败或跳过。",

    "dec_score_combined": "总分",
    "dec_score_total_title": "总分上限 120。权重：延迟 30% + 吞吐 20% + 资源 10% + 送达 10% + 发送阻塞 10% + 覆盖度 10% + 拓扑 10%，再乘 confidence（high 1.00 / solo·medium 0.99 / unknown·noisy 0.95）。",
    "dec_confidence_prefix": "置信度",

    "heatmap_title": "按消息大小比较延迟（P50 / P90 / P99，越低越好）",
    "heatmap_note": "每格三柱依次为 P50 / P90 / P99，柱越短越绿越好（独立按各百分位绝对分着色）。右侧数值同顺序，单位 μs。空白 = 无数据，FAIL = 用例失败。",
    "heatmap_latency_good": "延迟低",
    "heatmap_latency_mid": "延迟中等",
    "heatmap_latency_bad": "延迟高",
    "heatmap_throughput_title": "按消息大小比较吞吐（接收 MB/s，越高越好）",
    "heatmap_throughput_note": "这里只看吞吐测试。每行是一个测试对象，每列是一种消息大小。格子里的数字是平均接收吞吐 MB/s；横条越长、颜色越绿，表示订阅端实际收到的数据越多。空白或失败表示没有测到有效结果。",
    "heatmap_head_transport": "测试对象 \\ 消息大小",

    "profile_title": "测试对象推荐",
    "profile_note": "按本次总分从高到低排列。每张卡对应一个被测 URL/传输配置；先看总分、延迟、吞吐、CPU 和置信度，点开后看 URL、运行方式和典型指标。",
    "profile_fact_url_prefix": "协议/传输",
    "profile_fact_deployment": "运行方式",
    "profile_num_best_p95": "最好 P95 延迟",
    "profile_num_best_send_block": "发送阻塞 P99 最优",
    "profile_num_peak": "最高接收吞吐",
    "profile_num_avg_cpu": "平均 CPU",
    "profile_num_cases": "参与统计",
    "profile_num_cpu_ctx": "发布端 + 订阅端",
    "profile_num_loss": "丢包率",
    "profile_num_loss_ctx": "聚合/最坏用例",
    "profile_top_pick_prefix": "本次优先看：",
    "profile_top_pick_reason": "它是本次测试里综合表现最强的结果；继续结合下方延迟、吞吐和传输健康确认细节。",

    "layer_inprocess": "同进程",
    "layer_crossprocess": "同机跨进程",
    "layer_unknown": "未知",

    "scenario_transport_title": "传输详细结果",
    "scenario_serialization_title": "序列化详细结果",

    "suite_score_title": "分项结果",
    "executive_title": "结果亮点",
    "planner_notes_title": "运行备注",
    "transport_rollup_title": "传输汇总",
    "transport_rollup_note": "这里把同一个 URL/传输配置的结果汇总到一行，方便快速看趋势。一行里可能包含不同消息大小、速率和拓扑，精确比较请看延迟/吞吐对比图和趋势图。",
    "serialization_overview_title": "序列化概览",
    "methodology_show": "查看方法说明",
    "methodology_hide": "隐藏方法说明",

    "overview_note": "报告会把同一用例的重复运行合并统计，平均值只计算成功运行。丢包率达到 5% 才会在概要、健康面板和推荐卡片标题上突出显示，避免正常轻微丢包干扰主结论；展开测试对象推荐卡片可以查看每条 URL 完整的丢包率数值（不论是否触发阈值）。",

    "metrics_title": "指标解释",
    "metrics_note": "这里解释报告里最常见、也最容易误解的指标。读懂这些词，就能更准确地判断推荐排名、延迟/吞吐对比图和分项结果。",
    "th_metric": "指标",
    "th_meaning": "含义",
    "metric_p95_label": "P50 / P90 / P99 延迟（详细表里还有 P99.9 / P99.99）",
    "metric_p95_meaning": "延迟百分位。热力图展示 P50（典型）/ P90（偏慢的常见值）/ P99（尾部）三档；详细表与评分还会引入 P99.9、P99.99，用来暴露极少数慢消息。数值越小越好。",
    "metric_mbps_label": "接收吞吐 MB/s",
    "metric_mbps_meaning": "订阅端实际收到的数据量，单位是每秒 MB。它是吞吐测试最重要的数字。",
    "metric_loss_label": "丢包率",
    "metric_loss_meaning": "应该收到但没有收到的消息。压力测试里轻微丢包可能出现；报告只突出 5% 及以上的丢包，过高丢包会更明显标红。",
    "metric_confidence_label": "置信度",
    "metric_confidence_meaning": "用来提示结果是否稳定可信，主要看运行是否成功、是否有明显丢包、是否有延迟采样丢弃。置信度低表示需要查看传输健康和异常用例，最终仍要结合延迟和吞吐判断。",
    "metric_payload_label": "消息大小/载荷",
    "metric_payload_meaning": "每条消息携带的数据大小和类型。消息越大，越容易考验带宽、缓冲区和拷贝开销。",
    "metric_topology_label": "连接拓扑",
    "metric_topology_meaning": "发布者和订阅者的连接形态，例如 1:1、1:n、n:1、n:n。它用来观察连接数量增加后性能是否还能保持。",
    "metric_cpu_label": "CPU 效率",
    "metric_cpu_meaning": "发布端和订阅端消耗 CPU 后换来的吞吐表现。它用于辅助判断和区分接近的结果，延迟和吞吐仍是主指标。",

    "th_transport": "传输",
    "th_mode": "模式",
    "th_url": "URL",
    "th_modes": "模式列表",
    "th_passing": "通过",
    "th_warning": "警告",
    "th_failing": "失败",
    "th_unstable_cases": "异常用例",
    "th_total_cases": "总用例数",
    "th_best_recv_mbps": "最佳接收 MB/s",
    "th_best_p95_us": "最佳 P95 (us)",
    "th_suite": "测试类别",
    "th_winner": "表现最好",
    "th_score": "分数",
    "th_coverage": "覆盖度",
    "th_conclusion": "结论",
    "th_input_factors": "评分重点",
    "th_gates": "怎么看",
    "th_repeats": "重复次数",
    "th_recv_mbps": "接收 MB/s",
    "th_mean_p95": "平均 P95 (us)",
    "th_mean_p999": "平均 P99.9 (us)",
    "th_send_block_p99": "发送阻塞 P99 us",
    "th_interpretation": "适合看什么",
    "th_payload": "载荷",
    "th_size": "大小",
    "th_encode_mbps": "编码 MB/s",
    "th_decode_mbps": "解码 MB/s",
    "th_status": "状态",
    "th_config": "配置",
    "th_error": "错误",
    "th_rate": "速率",
    "th_loss_pct": "丢包率",
    "th_cpu_pct": "CPU 占用",
    "th_peak_rss_mb": "峰值 RSS MB",

    "chart_zoom_in": "放大",
    "chart_zoom_out": "缩小",
    "chart_reset": "重置视图",
    "chart_reset_short": "重置",
    "chart_hint": "滚轮=缩放 · 拖拽=平移 · 双击=重置",

    "chart_throughput": "吞吐量",
    "chart_rate_sweep": "速率扫描",
    "chart_latency": "延迟",
    "chart_loss": "丢包",
    "chart_send_block": "发送阻塞",
    "chart_title_send_block": "发送阻塞：publish() P99 阻塞时长 / 载荷大小",
    "chart_axis_send_block_p99": "发送阻塞 P99 (us)",
    "chart_first_message": "首条消息",
    "chart_resource_cpu": "CPU 占用",
    "chart_resource_rss": "内存占用",
    "chart_topology_1n": "拓扑 1:n",
    "chart_topology_n1": "拓扑 n:1",
    "chart_topology_nn": "拓扑 n:n",
    "chart_serialize": "序列化",
    "chart_deserialize": "反序列化",

    "chart_title_throughput": "吞吐：实际接收 MB/s / 载荷大小",
    "chart_title_rate_sweep": "速率扫描：实际接收 MB/s / 发送速率",
    "chart_title_latency": "延迟：平均 P95 (us) / 载荷大小",
    "chart_title_loss": "丢包：丢包率 / 载荷大小",
    "chart_title_first_message": "首条消息 (ms) / 载荷大小",
    "chart_title_resource_cpu": "资源：CPU% / 载荷大小",
    "chart_title_resource_rss": "资源：峰值 RSS MB / 载荷大小",
    "chart_title_topology_1n": "拓扑 1:n：实际接收 MB/s / 订阅者数",
    "chart_title_topology_n1": "拓扑 n:1：实际接收 MB/s / 发布者数",
    "chart_title_topology_nn": "拓扑 n:n：实际接收 MB/s / 连接对数",
    "chart_title_serialize": "序列化：编码 MB/s / 载荷大小",
    "chart_title_deserialize": "反序列化：解码 MB/s / 载荷大小",

    "chart_axis_payload_size": "载荷大小",
    "chart_axis_offered_msgs": "发送速率 (msg/s)",
    "chart_axis_subscribers": "订阅者数",
    "chart_axis_publishers": "发布者数",
    "chart_axis_endpoint_pairs": "连接对数",
    "chart_axis_delivered_mbps": "实际接收 MB/s",
)JSON"
      u8R"JSON(    "chart_axis_p95_latency": "平均 P95 延迟 (us)",
    "chart_axis_loss_pct": "丢包率 (%)",
    "chart_axis_first_message": "首条消息 (ms)",
    "chart_axis_cpu_pct": "CPU (%)",
    "chart_axis_peak_rss": "峰值 RSS MB",
    "chart_axis_encode_mbps": "编码 MB/s",
    "chart_axis_decode_mbps": "解码 MB/s",

    "aria_sections": "章节导航",
    "aria_chart_controls": "图表控制",
    "aria_series_legend": "系列图例",
    "aria_language": "语言",

    "meta_version_label": "版本",
    "meta_created_label": "生成于",


    "suite_score_methodology_note_1": "这张表把吞吐、延迟、backpressure、拓扑、序列化分开评分。每一行只适合在同一类测试里比较，不建议把延迟分数和吞吐分数直接互相换算。",
    "suite_score_three_layers_prefix": "分数主要来自该类测试最重要的指标，同时参考少量健康信息，例如失败、明显丢包、延迟采样丢弃和重复运行是否成功。",

    "suite_score_factors_throughput": "以实际接收 MB/s 为主，少量参考收发效率和健康状态。",
    "suite_score_gates_throughput": "吞吐测试主要看实际收到多少数据。健康项只用于提醒明显异常，不会代替吞吐本身。",
    "suite_score_interp_throughput": "适合判断大载荷或高发送压力下，哪组 URL/传输配置能稳定传更多数据。",
    "suite_score_factors_latency": "以 P99 等尾部延迟为主，少量参考最大延迟、抖动、吞吐和健康状态。多 case 聚合时大 payload 权重更高。",
    "suite_score_gates_latency": "延迟测试主要看百分位：热力图展示 P50（典型）/ P90（偏慢的常见值）/ P99（尾部）；评分还会加权 P99.9 / P99.99 来惩罚罕见卡顿。数值越低越好。",
    "suite_score_interp_latency": "适合判断消息响应是否够快、慢消息是否可控。平均值可能很好看，但 P99 或 P99.9 高时，用户仍可能感到卡顿。",
    "suite_score_factors_backpressure": "以 publisher 侧 P99 发送阻塞为主，同时参考实际接收吞吐、收发效率和健康状态。",
    "suite_score_gates_backpressure": "backpressure 测试模拟慢消费者。publish() 阻塞时间越低越好，但仍要求消息可靠送达。",
    "suite_score_interp_backpressure": "适合发现订阅端落后时，哪些传输会卡住发布线程。",
    "suite_score_factors_topology": "以总接收吞吐和扩展效率为主，少量参考收发效率和健康状态。",
    "suite_score_gates_topology": "拓扑测试主要看订阅者或发布者变多后，总吞吐能不能跟上。",
    "suite_score_interp_topology": "适合判断一对多、多对一、多对多场景下的扩展表现。",
    "suite_score_factors_serialization": "以编码/解码速度为主，同时参考覆盖、重复成功、效率、均衡、CPU 和内存。",
    "suite_score_gates_serialization": "序列化测试主要看编码和解码速度，同时检查重复运行是否稳定。",
    "suite_score_interp_serialization": "适合判断数据格式本身的编解码开销，不代表完整通信链路性能。",

    "executive_highlights_note": "这里列出本次报告里最容易关注的亮点：最高吞吐、最低延迟、拓扑扩展和最快编解码。它们用于快速定位结果，最终选型仍建议结合推荐排名、热力图和异常用例一起看。",
    "insight_highest_1to1_throughput": "最高 1:1 吞吐",
    "insight_lowest_1to1_latency": "最低 1:1 延迟",
    "insight_highest_1n_throughput": "最高 1:n 总吞吐",
    "insight_highest_nn_throughput": "最高 n:n 总吞吐",
    "insight_fastest_encode": "最快编码",
    "insight_url_label": "URL",
    "insight_topology_label": "拓扑",
    "insight_pattern_label": "模式",
    "insight_recv_label": "接收",
    "insight_first_msg_label": "首条消息",
    "insight_loss_label": "丢包",
    "insight_subscribers_label": "订阅者",
    "insight_publishers_label": "发布者",
    "insight_serialize_label": "编码",
    "insight_deserialize_label": "解码",
    "insight_repeats_label": "重复",

    "executive_coverage_urls": "个 URL，",
    "executive_coverage_transports": "种传输，以及",
    "executive_coverage_modes": "种模式。",
    "executive_coverage_passing": "正常用例:",
    "executive_coverage_unstable": "异常用例:",
    "executive_coverage_sentence_prefix": "本次运行覆盖",
    "executive_broadest_prefix": "正常用例最多的组合是",
    "executive_broadest_in": "在",
    "executive_broadest_mode_with": "模式下，有",
    "executive_broadest_suffix": "个正常用例。",
    "executive_broadest_tie_prefix": "共有",
    "executive_broadest_tie_middle": "个传输的正常覆盖并列最多，各有",

    "conclusion_latency_prefix": "延迟最低：",
    "conclusion_loss_label": "丢包",
    "conclusion_topology_prefix": "拓扑扩展最好：",
    "conclusion_topology_recv": "接收",
    "conclusion_topology_scale": "规模效率",
    "conclusion_backpressure_prefix": "发布阻塞最低：",
    "conclusion_backpressure_send_block": "发送阻塞 P99",
    "conclusion_backpressure_recv": "接收",
    "conclusion_throughput_prefix": "吞吐最好：",
    "conclusion_throughput_recv": "接收",
    "conclusion_throughput_first_msg": "首条消息",
    "conclusion_serialization_prefix": "最佳编解码切片",
    "conclusion_serialization_encode": "编码",
    "conclusion_serialization_decode": "解码",

    "coverage_case_suffix": "个用例，",
    "coverage_repeat_success_suffix": "重复成功，",
    "coverage_urls_suffix": "个 URL",
    "coverage_repeat_short": "重复",

    "transport_rollup_note_long": "这里按传输类型、运行模式和测试类别汇总结果，方便快速查看每组配置的整体趋势。由于一行里可能包含多个消息大小或速率，精确比较时请再看延迟/吞吐对比图和趋势图。",

    "serialization_overview_note": "这里比较不同载荷大小下的编码和解码速度，适合判断序列化开销。",

    "th_mean_recv_mbps": "平均接收 MB/s",
    "th_mean_run_p95_us": "平均 P95 延迟 us",
    "th_avg_loss_pct": "丢包 %",
    "th_mean_peak_rss_mb": "平均峰值 RSS MB",
    "th_mean_encode_mbps": "平均编码 MB/s",
    "th_mean_decode_mbps": "平均解码 MB/s",
    "th_mean_encode_cpu_ms": "平均编码 CPU ms",
    "th_mean_decode_cpu_ms": "平均解码 CPU ms",
    "th_errors": "错误",

    "mode_local_direct": "同进程直连",
    "mode_local_loop": "同进程事件环",
    "mode_process": "同机跨进程",
    "mode_unknown": "未知",

    "pill_speed_label": "延迟得分",
    "pill_capacity_label": "吞吐得分",
    "pill_efficiency_label": "资源占用得分",
    "pill_coverage_label": "覆盖得分",
    "pill_speed_title": "延迟得分（满分 100），尾部延迟越低分数越高。在总分中占 30%。",
    "pill_capacity_title": "吞吐得分（满分 100），实际接收 MB/s 越高分数越高。在总分中占 20%。",
    "pill_efficiency_title": "资源占用得分（满分 100），单位 CPU 时间产出的吞吐越高分数越高。在总分中占 10%。",
    "pill_coverage_title": "覆盖得分（满分 100），通过用例数 ÷ 总用例数；在总分中占 10%。",
    "pill_loss_label": "丢包",
    "pill_loss_title": "丢包率",

    "profile_scoring_legend": "总分上限 120。分数越高，越适合当前测试组合；权重为 延迟 30% + 吞吐 20% + 资源 10% + 送达 10% + 发送阻塞 10% + 覆盖度 10% + 拓扑 10%，再乘 confidence 因子。子项 pill 依次展示：延迟 / 吞吐 / 资源占用 / 覆盖 / 送达 / 发送阻塞。",
    "pill_delivery_label": "送达分",
    "pill_delivery_title": "送达完整性子分（0-100），基于跨用例聚合的丢包率换算，丢包越低分数越高。在总分中占 10%。",
    "pill_send_block_label": "发送阻塞得分",
    "pill_send_block_title": "发送阻塞子分（0-100），每个用例 publish() 调用墙上钟阻塞时长聚合到 P99；阻塞越低分数越高。在总分中占 10%——惩罚那些「占着发布线程不放」的传输实现。",

    "profile_formula_title": "总分公式（满分 120）— 每个维度详细说明",
    "profile_formula_latency": "延迟 30%",
    "profile_formula_latency_desc": "— 订阅端端到端延迟。每个 case 内部按 P95×0.50 + P99×0.35 + P99.9×0.12 + P99.99×0.03 加权，再过 payload 感知曲线（excellent_us = clamp(80 + payload_KiB·0.40, 80, 1500)，poor = excellent×8，中间对数插值）；跨 case 用 **payload 加权几何平均**，权重 = clamp(1 + 0.27·log1p(payload_KiB), 1, 3) — 大 payload 是小 payload 的 1.5~3 倍权重，但被夹在 [1,3] 区间不再走极端。",
    "profile_formula_throughput": "吞吐 20%",
    "profile_formula_throughput_desc": "— 订阅端实际收到的 MB/s。每个 case：log1p(recv_mbps)·100 / log1p(1024)；跨 case 在 throughput 套件内做等权几何平均，topology 套件仅在没有 throughput 套件数据时作为退路。",
    "profile_formula_resource": "资源 10%",
    "profile_formula_resource_desc": "— 单位 CPU 时间产出的吞吐。每个 case：throughput / (pub_cpu_ms + sub_cpu_ms)，相对该 run 内的最高效率值归一到 0-100。分数越高 = 每毫秒 CPU 推出的字节越多。",
    "profile_formula_delivery": "送达 10%",
    "profile_formula_delivery_desc": "— 跨所有 case 的丢包率聚合：avg_delivery = 100 - loss%。loss > 5% 时另用 logistic 曲线乘性折减 per-case 延迟分（≤20% 轻、>20% 加速、最多 ×0.40），防 survivor bias。",
    "profile_formula_send_block": "发送阻塞 10%",
    "profile_formula_send_block_desc": "— publisher 调用 publish() 进入到返回的墙上钟阻塞时长。延迟和 backpressure case 的 P99 阻塞时间会过 payload 感知曲线（excellent = clamp(5 + payload_KiB·0.05, 5, 200) µs，poor = excellent×30）；跨 case 同样按 payload 加权几何平均。**惩罚那些占着发布线程不放的 API**；异步入队即返回的 API 会自然拿高分。",
    "profile_formula_coverage": "覆盖度 10%",
    "profile_formula_coverage_desc": "— 该 URL 通过的 case 数 ÷ 该 URL 总 case 数。作为独立维度，拓扑维度缺失时也用它的 clamp [60, 95] 做退路占位。",
    "profile_formula_topology": "拓扑 10%",
    "profile_formula_topology_desc": "— topology 套件结果 roll-up（1:n / n:1 / n:n）。该 URL 没跑 topology 套件时退路到 clamp(coverage, 60, 95)。",
    "profile_formula_equation": "total = (0.36·延迟 + 0.24·吞吐 + 0.12·资源 + 0.12·送达 + 0.12·发送阻塞 + 0.12·覆盖度 + 0.12·拓扑) · confidence_factor  （每个子分 0-100 → 最高总分 120）",
    "profile_formula_confidence_note": "Confidence 由聚合/最坏单次 loss% 和延迟样本 drop 推断：<5% high，<15% medium，≥15% noisy；solo 表示无对照，unknown 表示该 URL 全部 case 失败。乘数：high 1.00 / solo·medium 0.99 / unknown·noisy 0.95。",

    "confidence_high": "高",
    "confidence_medium": "中",
    "confidence_noisy": "低",
    "confidence_solo": "仅一组",
    "confidence_unknown": "未知",

    "suite_score_panel_note": "本表按测试类别列出表现最好的配置。不同类别关注点不同，请在同一类别里比较分数。",

    "status_ok": "通过",
    "status_warn": "警告",
    "status_fail": "失败",
    "status_drop_label": "丢样",

    "config_node_label": "节点",
    "config_pub_label": "发布",
    "config_sub_label": "订阅"
  })JSON");
}

}  // namespace vlink::bench::report::i18n
