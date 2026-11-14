import fs from "node:fs";
import path from "node:path";
import { fileURLToPath } from "node:url";

const Root = path.resolve(path.dirname(fileURLToPath(import.meta.url)), "../..");
const Dist = path.join(Root, "docs/dist");
const Content = path.join(Root, "docs/src/content/docs");
const Origin = "https://gmoddev.github.io";
const Errors = [];
function Files(Directory) {
  return fs.readdirSync(Directory, { withFileTypes: true }).flatMap(Entry =>
    Entry.isDirectory() ? Files(path.join(Directory, Entry.name)) : [path.join(Directory, Entry.name)]);
}
function Destination(Pathname) {
  const Relative = decodeURIComponent(Pathname).replace(/^\/gargantuan(?:\/|$)/, "");
  const Candidate = path.resolve(Dist, Relative);
  if (!Candidate.startsWith(Dist + path.sep) && Candidate !== Dist) return null;
  return [Candidate, path.join(Candidate, "index.html")].find(Option =>
    fs.existsSync(Option) && fs.statSync(Option).isFile());
}
const Decode = Text => Text.replaceAll("&amp;", "&").replaceAll("&quot;", '"').replaceAll("&#39;", "'");
const Cache = new Map();
function Ids(File) {
  if (!Cache.has(File)) Cache.set(File, new Set([...fs.readFileSync(File, "utf8").matchAll(/\bid=["']([^"']+)["']/g)].map(Match => Decode(Match[1]))));
  return Cache.get(File);
}
if (!fs.existsSync(Dist)) throw new Error("[Docs:Site] Build docs/dist first");
const Pages = Files(Dist).filter(File => File.endsWith(".html"));
const SourcePages = Files(Content).filter(File => /\.mdx?$/.test(File));
for (const Source of SourcePages) {
  const Slug = path.relative(Content, Source).replaceAll("\\", "/")
    .replace(/\.mdx?$/, "").replace(/(?:^|\/)index$/, "");
  if (!Destination(`/gargantuan/${Slug ? Slug + "/" : ""}`)) {
    Errors.push(`content page was not emitted: ${path.relative(Content, Source)}`);
  }
}
let Links = 0;
for (const Page of Pages) {
  const Relative = path.relative(Dist, Page).replaceAll("\\", "/").replace(/index\.html$/, "");
  const Base = new URL(`/gargantuan/${Relative}`, Origin);
  const Html = fs.readFileSync(Page, "utf8");
  for (const Match of Html.matchAll(/<a\b[^>]*?\bhref=["']([^"']*)["']/g)) {
    const Href = Decode(Match[1]);
    const Url = new URL(Href, Base);
    Links += 1;
    const Repo = Url.href.match(/^https:\/\/github\.com\/gmoddev\/gargantuan\/(?:blob|tree)\/main\/([^?#]+)/i);
    if (Repo && !fs.existsSync(path.join(Root, decodeURIComponent(Repo[1])))) {
      Errors.push(`${Relative}: missing repository target ${Href}`);
    }
    if (Url.origin !== Origin) continue;
    const Target = Destination(Url.pathname);
    if (!Target) Errors.push(`${Relative}: missing published route ${Href}`);
    else if (Url.hash && Target.endsWith(".html") && !Ids(Target).has(decodeURIComponent(Url.hash.slice(1)))) {
      Errors.push(`${Relative}: missing published anchor ${Href}`);
    }
  }
}
for (const Error of [...new Set(Errors)]) console.error(`[Docs:Site] ${Error}`);
console.log(`[Docs:Site] ${SourcePages.length} content pages, ${Pages.length} HTML pages, ${Links} links, ${Errors.length} errors`);
process.exitCode = Errors.length ? 1 : 0;
