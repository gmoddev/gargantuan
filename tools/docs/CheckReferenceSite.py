"""Validate built reference pages and qualified member anchors without a browser."""
import json
from pathlib import Path
import re
import sys
import GenerateReference as Reference

Root = Reference.Root
Dist = Root/"docs/dist"
with Reference.Snapshot(Root, Reference.Source) as SourceRoot:
    Previous = Reference.Docs.Root
    try:
        Reference.Docs.Root = SourceRoot
        Api = Reference.Docs.ExtractApi()
    finally:
        Reference.Docs.Root = Previous
for Entry in Api["Classes"]:
    Page = Dist/"reference/classes"/Entry["Name"].lower()/"index.html"
    Text = Page.read_text(encoding="utf-8")
    for Section in ("Properties", "Methods", "Signals"):
        for Member in Entry[Section]:
            Identity = Entry["Name"]+"."+Member["Name"]
            Anchor = re.sub(r"[^\w\- ]", "", Identity.lower()).replace(" ", "-")
            if f'id="{Anchor}"' not in Text or Identity not in Text:
                raise SystemExit("Missing qualified member " + Identity)
for File in Reference.Build():
    if File.is_relative_to(Reference.Content) and File.suffix==".md":
        Relative = File.relative_to(Root/"docs/src/content/docs").with_suffix("")
        if Relative.name=="index":
            Relative = Relative.parent
        if not (Dist/Relative/"index.html").exists():
            raise SystemExit("Missing rendered page "+str(Relative))
if not list(Dist.rglob("pagefind.js")):
    raise SystemExit("Missing search index")
Lifecycle = json.loads((Root/"docs/src/data/LifecycleApi.json").read_text(encoding="utf-8"))
for Entry in Lifecycle["Classes"]:
    Text = (Dist/"lifecycle-reference"/Entry["Name"].lower()/"index.html").read_text(encoding="utf-8")
    for Section in ("Properties", "Methods", "Signals"):
        for Member in Entry[Section]:
            Anchor = (Entry["Name"]+Member["Name"]).lower()
            if f'id="{Anchor}"' not in Text:
                raise SystemExit("Missing lifecycle member "+Anchor)
print("[Docs:Reference] All declared member anchors, generated pages and Pagefind output verified")
