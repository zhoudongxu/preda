"""Independent scoring and paired set differences, with all failures retained."""
from __future__ import annotations

import collections
import csv
import json
import pathlib
import statistics
from corpus import digest

MODES=["site_scan","cfg_icfg","formula_smt","full"]


def score(answer,truth):
    status=answer["status"];value=answer.get("value");gold=truth["truth"]
    if status=="decided":
        return "correct" if truth["truth_kind"]=="boolean" and isinstance(value,bool) and value==gold else "wrong"
    if status=="bound":
        return "correct" if truth["truth_kind"]=="resource_supremum" and gold is not None and isinstance(value,int) and not isinstance(value,bool) and value>=gold else "wrong"
    if status=="unbounded":
        return "correct" if truth["truth_kind"]=="resource_supremum" and gold is None else "wrong"
    if status in ["unknown","unsupported","timeout","execution_error"]:return status
    raise ValueError("unknown answer status: "+status)


def dump(path,obj):
    path.write_text(json.dumps(obj,indent=2,sort_keys=True)+"\n")


def csv_file(path,rows,fields):
    with path.open("w",newline="") as stream:
        writer=csv.DictWriter(stream,fieldnames=fields,extrasaction="ignore");writer.writeheader();writer.writerows(rows)


def report(output):
    output=pathlib.Path(output)
    corpus=json.loads((output/"corpus.json").read_text())
    oracle={q["query_id"]:q for q in json.loads((output/"oracle.json").read_text())}
    queries={q["id"]:q for q in corpus["queries"]}
    answers=json.loads((output/"answers.json").read_text())
    plan=json.loads((output/"run_plan.json").read_text());reps=plan["repetitions"]
    if len(oracle)!=len(queries) or set(oracle)!=set(queries):raise ValueError("oracle/query mismatch")
    expected={(q,m,r) for q in queries for m in MODES for r in range(reps)}
    keys=[(a["query_id"],a["mode"],a["repetition"]) for a in answers]
    if len(keys)!=len(set(keys)) or set(keys)!=expected:raise ValueError("missing or duplicate mode answers")
    samples=[];groups=collections.defaultdict(list)
    for a in answers:
        q=queries[a["query_id"]];gold=oracle[a["query_id"]]
        row=dict(a,property=q["property"],cohort=q["cohort"],truth=gold["truth"],score=score(a,gold),vacuous=gold["vacuous"])
        row["native_score"]=score(a["native_answer"],gold) if "native_answer" in a else None
        row["tight"]=(a["status"]=="bound" and row["score"]=="correct" and a["value"]==gold["truth"])
        row["slack"]=a["value"]-gold["truth"] if a["status"]=="bound" and row["score"]=="correct" else None
        row["ratio"]=(a["value"]/gold["truth"]) if row["slack"] is not None and gold["truth"]>0 else None
        samples.append(row);groups[(a["query_id"],a["mode"])].append(row)
    primary=[r for r in samples if r["repetition"]==0]
    unstable=[]
    for (qid,mode),rows in groups.items():
        if len({(r["status"],r["value"]) for r in rows})>1:
            unstable.append(dict(query_id=qid,mode=mode,outcomes=[dict(repetition=r["repetition"],status=r["status"],value=r["value"],score=r["score"]) for r in rows]))
    summaries=[]
    cohort_values=["all"]+sorted({q["cohort"] for q in queries.values()})
    property_values=["all"]+sorted({q["property"] for q in queries.values()})
    for cohort in cohort_values:
        for prop in property_values:
            for mode in MODES:
                selected=[r for r in primary if r["mode"]==mode and (cohort=="all" or r["cohort"]==cohort) and (prop=="all" or r["property"]==prop)]
                if not selected:continue
                counts=collections.Counter(r["score"] for r in selected)
                finite=[r for r in selected if r["property"] in ["direct_work","tree_work","depth"] and r["truth"] is not None]
                bound=[r for r in finite if r["score"]=="correct" and r["status"]=="bound"]
                row=dict(mode=mode,cohort=cohort,property=prop,total=len(selected),
                         **{k:counts[k] for k in ["correct","wrong","unknown","unsupported","timeout","execution_error"]})
                row.update(correct_fraction=counts["correct"]/len(selected),finite_bound_denominator=len(finite),finite_bound_covered=len(bound),
                           tight_bound_count=sum(r["tight"] for r in bound),native_correct=sum(r["native_score"]=="correct" for r in selected),
                           stable_queries=sum((r["query_id"],mode) not in {(u["query_id"],u["mode"]) for u in unstable} for r in selected),
                           median_positive_bound_ratio=statistics.median([r["ratio"] for r in bound if r["ratio"] is not None]) if any(r["ratio"] is not None for r in bound) else None)
                summaries.append(row)
    increments=[]
    for cohort in cohort_values:
        for mode in MODES[:-1]:
            chosen=[r for r in primary if cohort=="all" or r["cohort"]==cohort]
            baseline={r["query_id"] for r in chosen if r["mode"]==mode and r["score"]=="correct"}
            full={r["query_id"] for r in chosen if r["mode"]=="full" and r["score"]=="correct"}
            increments.append(dict(cohort=cohort,baseline=mode,full_new=sorted(full-baseline),baseline_only=sorted(baseline-full),both=sorted(baseline&full)))
    # Different generated source hashes would invalidate a layer comparison.
    source_checks=[];phase_checks=[]
    for p in corpus["programs"]:
        processes=[json.loads((output/"runs"/p["id"]/m/("rep_%02d"%r)/"process.json").read_text()) for m in MODES for r in range(reps)]
        hashes={r.get("generated_cpp_sha256") for r in processes}
        source_checks.append(dict(program=p["id"],successful_runs=sum(r["status"]=="ok" for r in processes),
                                 expected_runs=4*reps,identical_generated_cpp=len(hashes)==1 and None not in hashes))
        if digest((output/p["path"]).read_bytes())!=p["source_sha256"]:raise ValueError("source hash mismatch")
        for process in processes:
            mode=process["mode"];r=process["repetition"]
            if process.get("binaries")!=plan["binaries"]:raise ValueError("binary mismatch")
            if process["status"]!="ok":continue
            metrics=json.loads((output/"runs"/p["id"]/mode/("rep_%02d"%r)/"metrics.json").read_text())
            f=metrics["build_features"];phases=metrics["analysis_phases"]
            expected_phases={"cfg_construction":mode!="site_scan","refinement_generation":mode in ["formula_smt","full"],"certificate_generation":mode=="full"}
            checks=[(phases[k]["invocations"]>0)==v for k,v in expected_phases.items()]
            passed=all(checks) and f["z3"] and f["analysis_profiling"] and not f["runtime_trace"] and not f["runtime_optimization"] and metrics["analysis_mode"]==mode
            phase_checks.append(dict(program=p["id"],mode=mode,repetition=r,passed=passed))
            if not passed:raise ValueError("mode/phase/build check failed")
    errors=[r for r in samples if r["score"]=="wrong" or r["native_score"]=="wrong"]
    summary=dict(query_count=len(queries),program_count=len(corpus["programs"]),entry_count=len(corpus["entries"]),
                 mode_query_observations=len(samples),primary_repetition=0,repetitions=reps,
                 summaries=summaries,incremental= increments,unstable=unstable,error_count=len(errors),
                 errors=[{k:r.get(k) for k in ["query_id","mode","repetition","value","truth","score","native_score","origin"]} for r in errors],
                 source_checks=source_checks,phase_checks=phase_checks)
    dump(output/"summary.json",summary);dump(output/"scored_answers.json",samples);dump(output/"errors.json",errors)
    csv_file(output/"query_results.csv",primary,["query_id","mode","cohort","property","status","value","truth","score","origin","native_score","tight","slack","ratio","vacuous","reason"])
    csv_file(output/"all_repetitions.csv",samples,["query_id","mode","repetition","status","value","truth","score","origin","native_score","tight"])
    csv_file(output/"effectiveness.csv",summaries,list(summaries[0]))
    ledger=[]
    for q in queries.values():
        truth=oracle[q["id"]]
        ledger.append(dict(query_id=q["id"],entry=q["entry_id"],cohort=q["cohort"],property=q["property"],site_a=q.get("site_a"),site_b=q.get("site_b"),truth=truth["truth"],truth_kind=truth["truth_kind"],vacuous=truth["vacuous"],enumerated_input_classes=truth["enumerated_input_classes"]))
    csv_file(output/"query_registry.csv",ledger,list(ledger[0]))
    native_rows=[dict(query_id=r["query_id"],property=r["property"],cohort=r["cohort"],status=r["native_answer"]["status"],value=r["native_answer"]["value"],truth=r["truth"],score=r["native_score"],tight=(r["native_answer"]["status"]=="bound" and r["native_answer"]["value"]==r["truth"]),reason=r["native_answer"]["reason"]) for r in primary if "native_answer" in r]
    csv_file(output/"native_certificate_results.csv",native_rows,list(native_rows[0]))
    reference=["# Source-to-oracle audit ledger","", "The models below were frozen before compiler answers. They use no compiler IR, certificate output, or SMT solver. They are author-constructed models; an independent human audit remains useful.",""]
    for entry in corpus["entries"]:
        prog=next(p for p in corpus["programs"] if p["id"]==entry["program_id"])
        reference += ["## "+entry["id"],"", "Source: `"+prog["path"]+"`", "", "Source SHA-256: `"+prog["source_sha256"]+"`", "", "Function: `"+entry["function_signature"]+"`", "",entry["oracle_model"]["justification"],"", "Locators:","```json",json.dumps(entry["sites"],indent=2,sort_keys=True),"```", "", "Independent model:","```json",json.dumps(entry["oracle_model"],indent=2,sort_keys=True),"```",""]
    (output/"REFERENCE_MODELS.md").write_text("\n".join(reference)+"\n")
    inc_rows=[]
    for x in increments:
        for role in ["full_new","baseline_only","both"]:
            inc_rows.extend(dict(cohort=x["cohort"],baseline=x["baseline"],set=role,query_id=q) for q in x[role])
    csv_file(output/"incremental_queries.csv",inc_rows,["cohort","baseline","set","query_id"])
    headline=[r for r in summaries if r["cohort"]==r["property"]=="all"]
    vs_smt=next(x for x in increments if x["cohort"]=="all" and x["baseline"]=="formula_smt")
    conclusion=("full 与 Formula/SMT 正确解决的查询集合完全相同。此次实验没有显示证书层对强 SMT 基线的额外语义查询覆盖收益；不得将原生证书输出数量解释为新增解决的问题数量。" if not vs_smt["full_new"] and not vs_smt["baseline_only"] else
                "full 相对 Formula/SMT 新增解决 %d 个查询，同时有 %d 个查询仅由 Formula/SMT 解决。"%(len(vs_smt["full_new"]),len(vs_smt["baseline_only"])))
    text=["# R-PREDA 统一查询效果实验", "", "**关键结果："+conclusion+"**", "", "本报告由冻结查询、独立参考模型和逐模式运行结果生成。主表固定使用第 0 次运行；其他两次用于稳定性核对，不挑选最优结果。", "",
          "## 规模与分母", "", "- %d 份程序快照，%d 个入口，%d 个查询。"%(summary["program_count"],summary["entry_count"],summary["query_count"]),
          "- 四种模式，每种三次独立进程运行，共 %d 个逐查询观测。"%len(samples),
          "- 现有合约来自 4 份源文件；原始入口、此前人工加入的并行扩展入口、源代码变异和受控程序分别报告。", "",
          "## 主结果", "", "| 模式 | 正确结论/安全上界 | 错误 | Unknown | Unsupported | Timeout | 执行错误 |", "|---|---:|---:|---:|---:|---:|---:|"]
    for r in headline:text.append("| {mode} | {correct}/{total} | {wrong} | {unknown} | {unsupported} | {timeout} | {execution_error} |".format(**r))
    text += ["", "正确包含正确的正/反结论和安全有限上界；安全上界不等于精确值。Unsupported 保留在统一分母中。", "",
             "## 分组结果", "", "| 样本组 | 查询数 | site_scan 正确 | CFG/ICFG 正确 | Formula/SMT 正确 | full 正确 |", "|---|---:|---:|---:|---:|---:|"]
    for cohort in cohort_values[1:]:
        rows=[r for r in summaries if r["cohort"]==cohort and r["property"]=="all"]
        text.append("| %s | %d | %s |"%(cohort,rows[0]["total"]," | ".join(str(r["correct"]) for r in rows)))
    text += ["", "existing_extension 是此前为证书实验加入现有合约的入口，existing_original 是原始入口；两者不能合称独立生产合约样本。", "",
             "## full 的逐查询增量", "", "| 对照 | full 新增解决 | 对照独有解决 | 双方解决 |", "|---|---:|---:|---:|"]
    for x in increments:
        if x["cohort"]=="all":text.append("| %s | %d | %d | %d |"%(x["baseline"],len(x["full_new"]),len(x["baseline_only"]),len(x["both"])))
    text += ["", "增量由查询 ID 集合求差，完整清单在 `incremental_queries.csv`。它不能由两行总数直接相减替代。", "",
             "## 资源界", "", "| 模式 | 安全有限界/有限真值查询 | 精确界 | 正值界比例中位数 |", "|---|---:|---:|---:|"]
    for r in headline:text.append("| %s | %d/%d | %d | %s |"%(r["mode"],r["finite_bound_covered"],r["finite_bound_denominator"],r["tight_bound_count"],r["median_positive_bound_ratio"] if r["median_positive_bound_ratio"] is not None else "—"))
    text += ["", "零真值查询单独以 slack 和 exactness 衡量，不计算除零比例。无界真值保留在主表，不混入有限界覆盖率分母。", "",
             "## 原生证书与稳定性", ""]
    for r in headline:
        if r["mode"]=="full":text.append("full 的原生证书直接提供 %d 个正确答案；其余可由相同的较低层规则回答。原生覆盖率与整体查询能力分别记录。"%r["native_correct"])
    text += ["", "- 逐查询结果不稳定项：%d。"%len(unstable),"- 包含原生答案在内的错误观测：%d。"%len(errors),
             "- 四种模式生成代码均一致的程序：%d/%d。"%(sum(r["identical_generated_cpp"] for r in source_checks),len(source_checks)), "",
             "## 可支持的结论与局限", "",
             "本实验回答这些 PREDA-native 方法在同一查询集合上的有效性。它不构成第三方合约分析器的跨工具比较，也不评价调度加速、Native 执行正确性或完整 FSE 实验充分性。", "",
             "参考答案来自独立于编译器 IR 和 full 结果的源级模型及有限商域穷举；它不调用 Z3。真实合约的源级抽象包含明确的数学论证，但目前未经过另一名人工审阅者独立审核。源码、模型、语义定位和具体反例全部保留，便于核查。", "",
             "受控模板和作者加入的并行扩展占较大比例，不能把查询数当作独立应用数量。主表应与按 cohort/property 分层的 `effectiveness.csv` 一起报告。", "",
             "这次运行用于效果比较，未执行性能预热；耗时仅为过程记录。成本结论应使用此前独立的消融开销矩阵。"]
    (output/"REPORT.md").write_text("\n".join(text)+"\n")
    tex=[r"\begin{tabular}{lrrrrr}",r"\hline",r"Mode & Correct & Wrong & Unknown & Unsupported & Timeout \\",r"\hline"]
    for r in headline:tex.append("%s & %d/%d & %d & %d & %d & %d \\\\"%(r["mode"].replace("_",r"\_"),r["correct"],r["total"],r["wrong"],r["unknown"],r["unsupported"],r["timeout"]))
    tex += [r"\hline",r"\end{tabular}"]
    (output/"paper_table.tex").write_text("\n".join(tex)+"\n")
    by_mode={r["mode"]:r for r in headline}
    paper=("We compared four PREDA-native analysis modes on %d fixed queries from %d entry slices in %d source snapshots. "
           "The corpus separates controlled programs, original contract entries, authored parallel extensions, and source mutants. "
           "The existing entries come from four contract sources. We used the same source and compiler binary for all modes. "
           "Each configuration ran three times. The main table uses the first run; the other runs check stability.\n\n"
           "An independent source-level model defines the reference answer for each query. It enumerates finite input classes "
           "that preserve the queried properties. Two unbounded patterns use explicit cycle arguments. The oracle does not "
           "read compiler output or use an SMT solver. These models are authored reference models and have not had a second human audit.\n\n"
           "The site scanner answered %d queries correctly. CFG/ICFG answered %d. Formula/SMT and the full mode answered %d and %d. "
           "No wrong answers were observed against these reference models. Unknown and unsupported cases remain in the denominator. "
           "All primary answer statuses and values were stable across the three runs.\n\n"
           "%s The native certificates directly supported %d query answers. This is a count of supported queries, not a count of new semantic capabilities or certificate objects. "
           "The experiment measures semantic query coverage. It does not measure Native execution, certificate reuse, binding validation, or scheduler performance.\n") % (
               len(queries),len(corpus["entries"]),len(corpus["programs"]),by_mode["site_scan"]["correct"],by_mode["cfg_icfg"]["correct"],by_mode["formula_smt"]["correct"],by_mode["full"]["correct"],
               "Formula/SMT and full solved the same set of queries. Thus, the certificate layer added no query coverage over this strong SMT baseline on this corpus." if not vs_smt["full_new"] and not vs_smt["baseline_only"] else "The per-query set differences are reported separately.",by_mode["full"]["native_correct"])
    (output/"PAPER_TEXT.md").write_text(paper)
    print(json.dumps({"headlines":headline,"errors":len(errors),"unstable":len(unstable),"new_vs_formula":[len(x["full_new"]) for x in increments if x["cohort"]=="all" and x["baseline"]=="formula_smt"]},indent=2))
    return summary
