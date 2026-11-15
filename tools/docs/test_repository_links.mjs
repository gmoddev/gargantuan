import assert from "node:assert/strict";
import path from "node:path";
import { fileURLToPath } from "node:url";
import test from "node:test";
import { ResolveLink } from "../../docs/plugins/repository-links.mjs";

const Root = path.resolve(path.dirname(fileURLToPath(import.meta.url)), "../..");
const Content = path.join(Root, "docs/src/content/docs");
test("directory index links remain on the documentation site", () => {
  assert.equal(ResolveLink(path.join(Content, "index.mdx"), "./api/"), "/gargantuan/api/");
});
test("checkout and route relative sibling links share a canonical route", () => {
  const Source = path.join(Content, "developing/physics.mdx");
  assert.equal(ResolveLink(Source, "./renderer/"), "/gargantuan/developing/renderer/");
  assert.equal(ResolveLink(Source, "../renderer/#limits"), "/gargantuan/developing/renderer/#limits");
});
test("source and engineering targets are linked to their owning repository", () => {
  assert.equal(ResolveLink(path.join(Content, "developing/physics.mdx"), "../../../../../src/Engine.cpp"),
    "https://github.com/gmoddev/gargantuan/blob/main/src/Engine.cpp");
});
test("external URLs and local anchors preserve identity", () => {
  for (const Url of ["#limits", "https://example.com/a", "mailto:owner@example.com"]) {
    assert.equal(ResolveLink(path.join(Content, "index.mdx"), Url), Url);
  }
});
test("missing targets and repository escapes fail", () => {
  const Source = path.join(Content, "index.mdx");
  assert.throws(() => ResolveLink(Source, "./definitely-absent/"), /missing target/);
  assert.throws(() => ResolveLink(Source, "../../../../../../"), /repository escape/);
});
