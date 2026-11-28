"""Normalize bounded declarative string helpers without executing Luau."""
import json
import re

def Expand(Text):
    Text = Text.replace("\r\n", "\n")
    Functions = {M[1]: (M[2], M[3]) for M in re.finditer(r'local function (\w+)\((\w+)\)\n(.*?)\nend', Text, re.S)}
    def Evaluate(Expression, Values):
        Result = []
        while Expression.strip():
            Expression = Expression.strip()
            M = re.match(r'''("(?:\\.|[^"\\])*"|'(?:\\.|[^'\\])*'|\w+)''', Expression)
            if not M:
                raise ValueError("Unsupported type expression "+Expression)
            Token = M[1]
            Expression = Expression[M.end():].strip()
            if Token[0] in "\"'":
                Value = json.loads(Token) if Token[0]=='"' else Token[1:-1]
            elif Expression.startswith("("):
                Depth = 1
                I = 1
                Quote = None
                while Depth:
                    C = Expression[I]
                    if Quote:
                        if C == Quote and Expression[I-1] != "\\": Quote = None
                    elif C in "\"'": Quote = C
                    elif C == "(": Depth += 1
                    elif C == ")": Depth -= 1
                    I += 1
                Argument = Evaluate(Expression[1:I-1], Values)
                Parameter, Body = Functions[Token]
                Local = dict(Values, **{Parameter: Argument})
                for Variable, Expr in re.findall(r'(?:local|const) (\w+) = (.+)', Body):
                    Local[Variable] = Evaluate(Expr, Local)
                Value = Evaluate(re.search(r'return (.+)', Body)[1], Local)
                Expression = Expression[I:].strip()
            else:
                Value = Values[Token]
            Result.append(Value)
            if not Expression: break
            if not Expression.startswith(".."): raise ValueError(Expression)
            Expression = Expression[2:]
        return "".join(Result)
    Clean = re.sub(r'local function \w+\(\w+\)\n.*?\nend\n', '', Text, flags=re.S)
    Start = Clean.index("return c(")
    Values = {}
    for Name, Expr in re.findall(r'^(?:local|const) (\w+) = (.+)$', Clean[:Start], re.M):
        if not Expr.startswith(('require', 'function')):
            Values[Name] = Evaluate(Expr, Values)
    def Signature(M):
        return M[1] + json.dumps(Evaluate(M[2], Values)) + M[3]
    return re.sub(r'(luauSignature\s*=\s*)([^\n]+?)(\s*},?\s*$|,\s*$)', Signature, Clean, flags=re.M)

