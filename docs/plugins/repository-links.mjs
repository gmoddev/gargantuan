import fs from "node:fs";
import path from "node:path";
import { fileURLToPath } from "node:url";

const Root = path.resolve(path.dirname(fileURLToPath(import.meta.url)), "../..");
const Content = path.join(Root, "docs/src/content/docs");
const Repository = "https://github.com/gmoddev/gargantuan";

// Markdown is readable in the checkout and on GitHub. Publish content links as
// site routes and source/engineering references as repository URLs.
export function ResolveLink(Source, Target) {
  if (/^(?:[a-z][a-z0-9+.-]*:|\/\/|#)/i.test(Target)) return Target;
  const Match = Target.match(/^([^?#]*)(.*)$/);
  const Local = decodeURIComponent(Match[1]);
  const Suffix = Match[2];
  const Candidate = Local.startsWith("/")
    ? path.join(Content, Local.replace(/^\/gargantuan(?:\/|$)/, "/"))
    : path.resolve(path.dirname(Source), Local);
  const Route = path.basename(Source).startsWith("index.")
    ? path.dirname(Source) : Source.replace(/\.mdx?$/, "");
  const Candidates = [Candidate];
  if (!Local.startsWith("/")) Candidates.push(path.resolve(Route, Local));
  const Options = Candidates.flatMap(Current => [Current,
    Current.replace(/\/$/, "") + ".mdx", Current.replace(/\/$/, "") + ".md",
    path.join(Current, "index.mdx"), path.join(Current, "index.md")]);
  const Resolved = Options.find(Option => fs.existsSync(Option) && fs.statSync(Option).isFile())
    || Candidates.find(Option => fs.existsSync(Option) && fs.statSync(Option).isDirectory());
  if (!Resolved) throw new Error(`[Docs:Links] ${Source}: missing target ${Target}`);
  const Relative = path.relative(Root, Resolved).replaceAll("\\", "/");
  if (Relative.startsWith("../") || path.isAbsolute(Relative)) {
    throw new Error(`[Docs:Links] repository escape: ${Target}`);
  }
  const ContentRelative = path.relative(Content, Resolved).replaceAll("\\", "/");
  if (!ContentRelative.startsWith("../") && /\.mdx?$/.test(ContentRelative)) {
    const Slug = ContentRelative.replace(/\.mdx?$/, "").replace(/(?:^|\/)index$/, "");
    return `/gargantuan/${Slug ? Slug + "/" : ""}${Suffix}`;
  }
  const Kind = fs.statSync(Resolved).isDirectory() ? "tree" : "blob";
  return `${Repository}/${Kind}/main/${Relative.split("/").map(encodeURIComponent).join("/")}${Suffix}`;
}

export default function RepositoryLinks() {
  return (Tree, File) => {
    const Source = File.path;
    if (!Source || !path.resolve(Source).startsWith(Content + path.sep)) return;
    function Visit(Node) {
      if ((Node.type === "link" || Node.type === "definition") && Node.url) {
        Node.url = ResolveLink(Source, Node.url);
      }
      for (const Child of Node.children || []) Visit(Child);
    }
    Visit(Tree);
  };
}
