// Build print-ready HTML from the docs' Markdown, for headless-browser PDF
// printing (see build-pdf.ps1).
//
//   node build.js refs   <out.html> <a.md> <b.md> ...   one quickref per page
//   node build.js manual <out.html> <manual.md>         one document; each
//                                                        "## " section on a new page
//
// Handles the Markdown these docs actually use: headings, paragraphs, ordered
// and bullet lists (nested, with continuation paragraphs and images inside
// steps), tables, fenced code, horizontal rules, ![images](), inline <img>
// tags, **bold**, *italic*, `code` and [links](). Links print as their text.
const fs = require('fs');
const path = require('path');
const [mode, out, ...files] = process.argv.slice(2);
if (!['refs', 'manual'].includes(mode) || !out || !files.length) {
  console.error('usage: node build.js refs|manual <out.html> <file.md>...');
  process.exit(2);
}

const escHtml = (s) => s.replace(/&/g, '&amp;').replace(/</g, '&lt;').replace(/>/g, '&gt;');
function inline(s) {
  // Raw <img> tags pass through untouched; everything else is escaped.
  return s.split(/(<img\b[^>]*>)/).map((part, i) => {
    if (i % 2) return part;
    let t = escHtml(part);
    t = t.replace(/!\[([^\]]*)\]\(([^)]+)\)/g, '<img src="$2" alt="$1">');
    t = t.replace(/`([^`]+)`/g, '<code>$1</code>');
    t = t.replace(/\*\*([^*]+)\*\*/g, '<strong>$1</strong>');
    t = t.replace(/(^|[^*\w])\*([^*\s][^*]*)\*/g, '$1<em>$2</em>');
    t = t.replace(/\[([^\]]+)\]\([^)]+\)/g, '$1');
    return t;
  }).join('');
}

const ITEM = /^(\d+\.|-)\s+/;
const BLOCK_START = /^(#{1,3}\s|\||```|---\s*$|!\[|<img\b|\d+\.\s|-\s)/;

function convert(md) {
  const lines = md.replace(/\r/g, '').split('\n');
  const html = [];
  let i = 0;
  const indentOf = (l) => l.length - l.trimStart().length;

  // A list whose markers sit at indent `ind`. Each item is a sequence of
  // paragraphs and nested lists, kept in source order.
  function list(ind) {
    const ordered = /^\d+\./.test(lines[i].trim());
    const items = [];
    let cur = null, para = null;
    const flush = () => { if (para !== null) { cur.blocks.push({ p: para }); para = null; } };
    while (i < lines.length) {
      const l = lines[i], t = l.trim();
      if (!t) {
        // Blank line: the list continues if indented content (an image inside
        // a step, a second paragraph) or the next item at this level follows,
        // so a figure between steps doesn't restart the numbering.
        const nx = lines[i + 1];
        const more = nx && nx.trim() &&
          (indentOf(nx) > ind || (indentOf(nx) === ind && ITEM.test(nx.trim())));
        if (more) { if (cur) flush(); i++; continue; }
        break;
      }
      const li = indentOf(l);
      if (ITEM.test(t) && li === ind) {
        if (cur) flush();
        cur = { blocks: [] }; items.push(cur);
        para = t.replace(ITEM, '');
        i++;
      } else if (ITEM.test(t) && li > ind && cur) {
        flush();
        cur.blocks.push({ html: list(li) });
      } else if (li > ind && cur) {
        para = para === null ? t : para + ' ' + t;   // continuation of the current paragraph
        i++;
      } else break;
    }
    if (cur) flush();
    const tag = ordered ? 'ol' : 'ul';
    return `<${tag}>` + items.map((it) => '<li>' + it.blocks.map((b, k) =>
      b.html ? b.html : (k === 0 ? inline(b.p) : `<p>${inline(b.p)}</p>`)).join('') + '</li>').join('') + `</${tag}>`;
  }

  while (i < lines.length) {
    const l = lines[i], t = l.trim();
    if (!t) { i++; continue; }
    if (/^\*Print this page/.test(t)) { i++; continue; }      // meta line, not for paper
    let m;
    if ((m = t.match(/^(#{1,3})\s+(.*)$/))) { html.push(`<h${m[1].length}>${inline(m[2])}</h${m[1].length}>`); i++; continue; }
    if (/^---\s*$/.test(t)) { html.push('<hr>'); i++; continue; }
    if (t.startsWith('```')) {
      const code = []; i++;
      while (i < lines.length && !lines[i].trim().startsWith('```')) code.push(lines[i++]);
      i++;
      html.push(`<pre><code>${escHtml(code.join('\n'))}</code></pre>`);
      continue;
    }
    if (t.startsWith('|')) {
      const rows = [];
      while (i < lines.length && lines[i].trim().startsWith('|')) rows.push(lines[i++].trim());
      const cells = (r) => r.replace(/^\||\|$/g, '').split('|').map((c) => c.trim());
      const head = cells(rows[0]), body = rows.slice(2).map(cells);
      html.push('<table><thead><tr>' + head.map((c) => `<th>${inline(c)}</th>`).join('') + '</tr></thead><tbody>' +
        body.map((r) => '<tr>' + r.map((c) => `<td>${inline(c)}</td>`).join('') + '</tr>').join('') + '</tbody></table>');
      continue;
    }
    if (ITEM.test(t)) { html.push(list(indentOf(l))); continue; }
    if (/^(!\[|<img\b)/.test(t)) { html.push(`<p class="fig">${inline(t)}</p>`); i++; continue; }
    const para = [];
    while (i < lines.length && lines[i].trim() && (para.length === 0 || !BLOCK_START.test(lines[i].trim()))) para.push(lines[i++].trim());
    html.push(`<p>${inline(para.join(' '))}</p>`);
  }
  return html.join('\n');
}

const STYLE = `
  @page { size: Letter; margin: 0.6in 0.7in; }
  * { box-sizing: border-box; }
  body { font-family: "Segoe UI", Arial, sans-serif; font-size: 12pt; line-height: 1.38; color: #111; margin: 0; }
  h1 { font-size: 22pt; margin: 0 0 14pt; padding-bottom: 6pt; border-bottom: 3px solid #1f7a3a; }
  h2 { font-size: 16pt; margin: 18pt 0 6pt; color: #1f7a3a; }
  h3 { font-size: 13pt; margin: 14pt 0 4pt; }
  p { margin: 6pt 0; }
  ol, ul { margin: 4pt 0 8pt; padding-left: 22pt; }
  li { margin: 4pt 0; }
  li > ul, li > ol { margin: 2pt 0; }
  li > p { margin: 4pt 0; }
  code { font-family: Consolas, monospace; font-size: 10.5pt; background: #eef2ee; padding: 0 3pt; border-radius: 3px; }
  pre { background: #eef2ee; padding: 7pt 9pt; border-radius: 4px; white-space: pre-wrap; break-inside: avoid; }
  pre code { background: none; padding: 0; }
  table { border-collapse: collapse; width: 100%; margin: 6pt 0 10pt; font-size: 11pt; }
  tr { break-inside: avoid; }
  th, td { border: 1px solid #999; padding: 5pt 7pt; text-align: left; vertical-align: top; }
  th { background: #e4efe6; }
  img { max-width: 100%; }
  p.fig { text-align: center; break-inside: avoid; }
  p.fig img { max-height: 3.6in; }
  hr { display: none; }
`;

let body, extra = '', head = '';
if (mode === 'refs') {
  extra = `
  body { font-size: 13pt; }
  .page { break-after: page; position: relative; min-height: 9.6in; }
  .page:last-child { break-after: auto; }
  footer { position: absolute; bottom: 0; left: 0; right: 0; font-size: 9pt; color: #777; border-top: 1px solid #ccc; padding-top: 4pt; }`;
  body = files.map((f) => `<section class="page">\n${convert(fs.readFileSync(f, 'utf8'))}\n` +
    `<footer>Weigh Station quick reference &middot; full detail in the user manual</footer>\n</section>`).join('\n');
} else {
  // Relative image paths in the manual resolve against its own folder. Every
  // "## " section starts a page, which leaves page 1 as a cover: title,
  // photo and introduction.
  const base = 'file:///' + path.resolve(path.dirname(files[0])).replace(/\\/g, '/') + '/';
  head = `<base href="${base}">`;
  extra = `
  h2 { break-before: page; }`;
  body = convert(fs.readFileSync(files[0], 'utf8'));
}

fs.writeFileSync(out, `<!doctype html>
<html><head><meta charset="utf-8"><title>Weigh Station</title>${head}
<style>${STYLE}${extra}
</style></head><body>
${body}
</body></html>`);
console.log(`wrote ${out}: ${mode === 'refs' ? files.length + ' refs' : 'manual'}`);
