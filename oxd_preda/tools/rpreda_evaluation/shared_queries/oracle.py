"""Independent finite-quotient trace oracle. No compiler or solver dependency."""
from __future__ import annotations

import ast
import itertools


def expression(text, environment):
    if not isinstance(text, str):
        return text
    def run(n):
        if isinstance(n, ast.Name):
            return environment[n.id]
        if isinstance(n, (ast.Num, ast.NameConstant)):
            return n.n if isinstance(n, ast.Num) else n.value
        if hasattr(ast, "Constant") and isinstance(n, ast.Constant):
            return n.value
        if isinstance(n, ast.UnaryOp):
            v = run(n.operand)
            if isinstance(n.op, ast.Not): return not v
            if isinstance(n.op, ast.USub): return -v
        if isinstance(n, ast.BoolOp):
            values = [run(v) for v in n.values]
            return all(values) if isinstance(n.op, ast.And) else any(values)
        if isinstance(n, ast.BinOp):
            a, b = run(n.left), run(n.right)
            if isinstance(n.op, ast.Add): return a + b
            if isinstance(n.op, ast.Sub): return a - b
            if isinstance(n.op, ast.Mult): return a * b
        if isinstance(n, ast.Compare):
            a = run(n.left)
            for op, right in zip(n.ops, n.comparators):
                b = run(right)
                choices = {ast.Eq: a == b, ast.NotEq: a != b,
                           ast.Lt: a < b, ast.LtE: a <= b,
                           ast.Gt: a > b, ast.GtE: a >= b}
                if type(op) not in choices: raise ValueError("unsupported comparison")
                if not choices[type(op)]: return False
                a = b
            return True
        raise ValueError("unsupported oracle expression: %s" % ast.dump(n))
    return run(ast.parse(text, mode="eval").body)


def traces(program, environment):
    """Execute the reference model; an emit carries its independent child tree."""
    emitted = []
    def block(statements, env):
        for s in statements:
            kind = s[0]
            if kind == "emit":
                children = traces(s[4] if len(s) > 4 else [], dict(env))
                emitted.append({"site": s[1], "scope": s[2],
                                "target": expression(s[3], env), "children": children})
            elif kind == "if":
                arm = s[2] if expression(s[1], env) else (s[3] if len(s) > 3 else [])
                if block(arm, env): return True
            elif kind == "set":
                env[s[1]] = expression(s[2], env)
            elif kind == "repeat":
                for _ in range(s[1]):
                    if block(s[2], env): return True
            elif kind == "return":
                return True
            else:
                raise ValueError("unknown oracle operation %s" % kind)
        return False
    block(program, dict(environment))
    return emitted


def tree_size(events):
    return sum(1 + tree_size(e["children"]) for e in events)


def tree_depth(events):
    return max([0] + [1 + tree_depth(e["children"]) for e in events])


def environments(model):
    domains = model.get("domains", {})
    names = sorted(domains)
    for values in itertools.product(*(domains[name] for name in names)):
        yield dict(zip(names, values))


def evaluate(entry, queries):
    model = entry["oracle_model"]
    bool_queries = [q for q in queries if "site_a" in q]
    answers = {q["id"]: q["property"] != "may_alias" for q in bool_queries}
    witnesses, coemission, reached = {}, {q["id"]: False for q in bool_queries}, set()
    bounds = {"direct_work": 0, "tree_work": 0, "depth": 0}
    bound_witnesses, count = {}, 0
    for env in environments(model):
        events = traces(model["program"], env)
        count += 1
        ids = [e["site"] for e in events]
        reached.update(ids)
        for q in bool_queries:
            a, b = q["site_a"], q["site_b"]
            has_both = a in ids and b in ids
            coemission[q["id"]] |= has_both
            if q["property"] == "may_alias":
                value = any(x["scope"] == y["scope"] and x["target"] == y["target"]
                            for x in events if x["site"] == a
                            for y in events if y["site"] == b)
                if value:
                    answers[q["id"]] = True
                    witnesses.setdefault(q["id"], {"input": env, "direct_trace": events})
            else:
                value = not has_both if q["property"] == "mutual_exclusion" else all(
                    a in ids[:i] for i, site in enumerate(ids) if site == b)
                if not value:
                    answers[q["id"]] = False
                    witnesses.setdefault(q["id"], {"input": env, "direct_trace": events})
        vals = {"direct_work": len(events), "tree_work": tree_size(events), "depth": tree_depth(events)}
        for prop, value in vals.items():
            if value >= bounds[prop]:
                bounds[prop] = value
                bound_witnesses[prop] = {"input": env, "direct_trace": events}
    for prop in model.get("unbounded", []):
        bounds[prop] = None
        bound_witnesses[prop] = {"lasso_argument": model["unbounded_reason"]}
    rows = []
    for q in queries:
        pair = "site_a" in q
        value = answers[q["id"]] if pair else bounds[q["property"]]
        rows.append({"query_id": q["id"], "truth": value,
                     "truth_kind": "boolean" if pair else "resource_supremum",
                     "enumerated_input_classes": count,
                     "coemission_possible": coemission.get(q["id"]),
                     "vacuous": pair and q["property"] == "must_precede" and q["site_b"] not in reached,
                     "witness": witnesses.get(q["id"]) if pair else bound_witnesses[q["property"]],
                     "justification": model["justification"]})
    return rows
