"""Validate built reference pages and qualified member anchors without a browser."""
import json
from pathlib import Path
import re
import sys
import GenerateReference as Reference

Root = Reference.Root
Dist = Root/"docs/dist"
Api = Reference.Docs.ExtractApi()
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
print("[Docs:Reference] All declared member anchors, generated pages and Pagefind output verified")
