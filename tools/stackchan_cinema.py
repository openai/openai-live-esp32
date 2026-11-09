#!/usr/bin/env python3
"""Search or silently preview a prepared film pack on a laptop (stdlib only)."""

import argparse
import json
import os
import re
import threading
from collections import OrderedDict
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path

from prepare_stackchan_cinema import api, similarity, vector

PAGE = r"""<!doctype html><html lang="en"><meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>StackChan · Memory cinema</title>
<style>
:root{color-scheme:dark;font:16px system-ui;background:#101112;color:#e8e5db}
body{max-width:1080px;margin:6vh auto;padding:24px}header{display:flex;align-items:center;gap:12px}
.dot{width:7px;height:7px;border-radius:50%;background:#6ddcd5}h1{font-size:24px;font-weight:500;margin:0}
.sub{color:#8f9698;margin:12px 0 32px}main{display:grid;grid-template-columns:2fr 1fr;gap:28px}
canvas{width:100%;aspect-ratio:4/3;background:black;border-radius:16px;display:block}
input,button{font:inherit;border:1px solid #343b3d;border-radius:10px;background:#1b2022;color:inherit;padding:12px}
input{box-sizing:border-box;width:100%;margin-bottom:10px}button{cursor:pointer}button:hover{border-color:#6ddcd5}
.row{display:flex;gap:8px;flex-wrap:wrap}.chips{margin:20px 0}.chips button{font-size:13px;padding:7px 10px}
#description{line-height:1.5;color:#bdc4c3}#credit{font-size:13px;color:#9ba6a9}a{color:#79cfc9}
#mode{color:#e1b971;font-size:13px;letter-spacing:.08em;text-transform:uppercase;margin:20px 0 8px}
#matches{padding:0;list-style:none}#matches button{width:100%;text-align:left;margin:4px 0;padding:10px;font-size:13px}
small{color:#8f9698}@media(max-width:700px){main{grid-template-columns:1fr}body{margin:0 auto}}
</style><header><i class="dot"></i><h1>Memory cinema</h1></header>
<p class="sub">StackChan’s visual subconscious · conversation, archival counterparts, idle dreams · silent</p>
<main><section><canvas id="screen" width="320" height="240"></canvas><div id="mode">Loading archive</div>
<div id="description"></div><p id="credit"></p></section><aside>
<form id="search"><input id="query" maxlength="256" placeholder="An object, a feeling, a fragment…" aria-label="Visual association">
<div class="row"><button>Find a memory</button><button type="button" id="dream">Dream</button><button type="button" id="pause">Pause</button></div></form>
<div class="row chips"><button>curious cat</button><button>ocean waves</button><button>old telephone</button><button>feeling weightless</button><button>intricate machinery</button></div>
<small id="status"></small><ul id="matches"></ul><p><small>Search sends only the entered text to the configured embedding API. Dreaming and playback stay local. Archival matches are associations, not factual evidence.</small></p>
</aside></main><script>
const $=id=>document.getElementById(id), ctx=$('screen').getContext('2d');
let catalog, current=-1, generation=0, queryGeneration=0, dreaming=true, paused=false, recent=[];
const wait=ms=>new Promise(r=>setTimeout(r,ms));
async function play(id){
 const mine=++generation;current=id;const clip=catalog.clips[id];
 $('description').textContent=clip.caption.description;
 $('credit').replaceChildren();const a=document.createElement('a');a.textContent=clip.archive.sourceTitle;
 a.href=clip.provenance.page;a.target='_blank';a.rel='noopener noreferrer';$('credit').append(a,document.createTextNode(' · '+clip.archive.sourceYear+' · '+clip.caption.mood));
 $('mode').textContent=dreaming?'Idle dream':'Visual association';recent.unshift(id);recent=recent.slice(0,8);
 try{
 const response=await fetch('/media/'+String(id).padStart(4,'0')+'.SCF');if(!response.ok)throw Error('Clip unavailable');
 const data=await response.arrayBuffer(), view=new DataView(data);
 if(data.byteLength<32||view.getUint16(8,true)!==320||view.getUint16(10,true)!==240)throw Error('Invalid clip');
 const fps=view.getUint16(12,true), count=view.getUint16(14,true);if(fps<1||fps>20||count<1||count>144||data.byteLength>2097152||data.byteLength<32+count*8)throw Error('Invalid frame index');
 let start=performance.now(), last=-1;
 while(mine===generation){
   if(paused){let t=performance.now();await wait(50);start+=performance.now()-t;continue}
   const frame=Math.floor((performance.now()-start)*fps/1000);
   if(frame>=count)break;
   if(frame===last){await wait(8);continue}
   const offset=view.getUint32(32+frame*8,true), n=view.getUint32(36+frame*8,true);
   if(n>32768||offset>data.byteLength||n>data.byteLength-offset)throw Error('Invalid frame');
   const image=await createImageBitmap(new Blob([data.slice(offset,offset+n)],{type:'image/jpeg'}));
   if(mine===generation)ctx.drawImage(image,0,0,320,240);image.close();last=frame;
 }
 if(mine===generation){if(dreaming)dreamNext();else play(id)}
 }catch(e){$('status').textContent=e.message}
}
function dreamNext(){
 let ids=current>=0&&Math.random()>.2?catalog.clips[current].neighbors:catalog.clips.map(c=>c.index);
 let fresh=ids.filter(i=>!recent.includes(i));if(!fresh.length)fresh=catalog.clips.map(c=>c.index).filter(i=>!recent.includes(i));
 if(!fresh.length)fresh=ids;play(fresh[Math.floor(Math.random()*fresh.length)]);
}
async function search(query){
 if(!query.trim())return;const mine=++queryGeneration;$('status').textContent='Finding a visual association…';
 try{const response=await fetch('/api/search',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify({query})});
 const results=await response.json();if(!response.ok)throw Error(results.error||'Search failed');if(mine!==queryGeneration)return;
 $('matches').replaceChildren();for(const m of results){const b=document.createElement('button');b.textContent=catalog.clips[m.index].archive.sourceTitle+' · '+m.score.toFixed(3);b.onclick=()=>{dreaming=false;play(m.index)};$('matches').append(b)}
 $('status').textContent=catalog.clips.length+' clips · '+catalog.dimensions+' dimensions · cosine scores, not probabilities';dreaming=false;play(results[0].index);
 }catch(e){if(mine===queryGeneration)$('status').textContent=e.message}
}
$('search').onsubmit=e=>{e.preventDefault();search($('query').value)};
document.querySelectorAll('.chips button').forEach(b=>b.onclick=()=>{$('query').value=b.textContent;search(b.textContent)});
$('dream').onclick=()=>{++queryGeneration;dreaming=true;dreamNext()};
$('pause').onclick=()=>{paused=!paused;$('pause').textContent=paused?'Resume':'Pause'};
fetch('/catalog.json').then(r=>r.json()).then(c=>{catalog=c;$('status').textContent=c.clips.length+' prepared clips · no audio';dreamNext()}).catch(e=>$('status').textContent=e.message);
</script></html>"""


class Pack:
    def __init__(self, path, key):
        self.path, self.key = path.resolve(), key
        self.catalog = json.loads((path / "catalog.json").read_text())
        self.embeddings = json.loads((path / "embeddings.json").read_text())
        self.cache = OrderedDict()
        self.lock = threading.Lock()
        if self.catalog["caption_digest"] != self.embeddings["caption_digest"] or len(
            self.catalog["clips"]
        ) != len(self.embeddings["vectors"]):
            raise ValueError("Catalog and embedding index mismatch")

    def search(self, query):
        if not isinstance(query, str) or not 1 <= len(query) <= 256:
            raise ValueError("Enter 1–256 characters")
        if not self.lock.acquire(blocking=False):
            raise ValueError("Another search is still running")
        try:
            if query in self.cache:
                self.cache.move_to_end(query)
                return self.cache[query]
            if not self.key:
                raise ValueError(
                    "Search needs OPENAI_API_KEY or --key-file; Dream works offline"
                )
            result = api(
                "embeddings",
                {
                    "model": self.embeddings["model"],
                    "dimensions": self.embeddings["dimensions"],
                    "input": query,
                },
                self.key,
            )
            q = vector(result["data"][0]["embedding"], self.embeddings["dimensions"])
            ranking = sorted(
                (
                    {"index": i, "score": similarity(q, v)}
                    for i, v in enumerate(self.embeddings["vectors"])
                ),
                key=lambda r: -r["score"],
            )[:8]
            self.cache[query] = ranking
            if len(self.cache) > 32:
                self.cache.popitem(last=False)
            return ranking
        finally:
            self.lock.release()


def serve(pack, port):
    class Handler(BaseHTTPRequestHandler):
        def log_message(self, *args):
            pass

        def send(self, data, kind="application/json", status=200):
            self.send_response(status)
            self.send_header("Content-Type", kind)
            self.send_header("Content-Length", str(len(data)))
            self.send_header("X-Content-Type-Options", "nosniff")
            self.send_header("Cache-Control", "no-store")
            self.end_headers()
            try:
                self.wfile.write(data)
            except (BrokenPipeError, ConnectionResetError):
                pass

        def do_GET(self):
            if self.path == "/":
                return self.send(PAGE.encode(), "text/html; charset=utf-8")
            if self.path == "/catalog.json":
                return self.send(json.dumps(pack.catalog).encode())
            match = re.fullmatch(r"/media/(\d{4})\.SCF", self.path)
            if match and int(match[1]) < len(pack.catalog["clips"]):
                path = (pack.path / "CINEMA" / (match[1] + ".SCF")).resolve()
                if (
                    path.is_relative_to(pack.path)
                    and path.is_file()
                    and path.stat().st_size <= 2097152
                ):
                    return self.send(path.read_bytes(), "application/octet-stream")
            self.send(b'{"error":"Not found"}', status=404)

        def do_POST(self):
            expected = f"http://127.0.0.1:{port}"
            if (
                self.path != "/api/search"
                or self.headers.get("Origin", expected) != expected
                or self.headers.get("Content-Type") != "application/json"
            ):
                return self.send(b'{"error":"Forbidden"}', status=403)
            try:
                size = int(self.headers.get("Content-Length", "0"))
                if not 1 <= size <= 2048:
                    raise ValueError("Invalid request length")
                data = json.loads(self.rfile.read(size))
                results = pack.search(data.get("query"))
                self.send(json.dumps(results).encode())
            except (ValueError, RuntimeError, KeyError, AttributeError):
                self.send(
                    b'{"error":"Search unavailable; check key, connection, or another pending search"}',
                    status=400,
                )

    print(f"Silent viewer: http://127.0.0.1:{port}/", flush=True)
    ThreadingHTTPServer(("127.0.0.1", port), Handler).serve_forever()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("pack", type=Path)
    parser.add_argument("--key-file", type=Path)
    parser.add_argument("--query")
    parser.add_argument("--port", type=int, default=8878)
    args = parser.parse_args()
    key = (
        args.key_file.read_text().strip()
        if args.key_file
        else os.environ.get("OPENAI_API_KEY", "").strip()
    )
    pack = Pack(args.pack, key)
    if args.query:
        print(
            json.dumps(
                [
                    {
                        **r,
                        "title": pack.catalog["clips"][r["index"]]["archive"][
                            "sourceTitle"
                        ],
                        "description": pack.catalog["clips"][r["index"]]["caption"][
                            "description"
                        ],
                    }
                    for r in pack.search(args.query)
                ],
                indent=2,
            )
        )
    else:
        serve(pack, args.port)


if __name__ == "__main__":
    main()
