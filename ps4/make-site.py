#!/usr/bin/env python3
"""Build the download-and-licences page that ships next to .index-extended.

⚠ THIS PAGE IS A LICENCE DOCUMENT BEFORE IT IS A DOWNLOAD PAGE. RetroArch is GPLv3 and most
cores are GPL of some vintage, so distributing the binaries obliges us to point at the
CORRESPONDING source - not "the project", the exact commit each binary was built from. Three
inputs have to agree for that to be true, and they only all exist inside the publish job:

    .index-extended            what was actually published, and under what filename
    cores-manifest-*.tsv       the commit sha each core was built from
    the libretro-super recipe  the repository that sha belongs to

Generating this anywhere else means fetching those across a repository boundary, where they can
drift - and a page claiming a sha that does not match the binary beside it is worse than no page,
because both numbers look equally true.

Licences come from the .info bundle rather than being hardcoded: 101 cores carry about 25
distinct licence strings between them, a fifth of them non-commercial in some form, and that is
not a fact any of us should be retyping.
"""

import argparse
import base64
import datetime
import glob
import html
import os
import re
import subprocess
import sys
import zipfile


def read_index(path):
    """[(date, crc, filename, core_name)] in the order the index lists them."""
    out = []
    with open(path, encoding="utf-8") as fh:
        for line in fh:
            parts = line.split()
            if len(parts) != 3:
                continue
            date, crc, filename = parts
            name = filename
            for suffix in (".zip", ".prx"):
                if name.endswith(suffix):
                    name = name[: -len(suffix)]
            if name.endswith("_libretro"):
                name = name[: -len("_libretro")]
            out.append((date, crc, filename, name))
    return out


def read_manifests(dirs):
    """core -> (result, size, sha). Later files win; shards do not overlap."""
    man = {}
    for d in dirs:
        for path in sorted(glob.glob(os.path.join(d, "**", "cores-manifest-*.tsv"),
                                     recursive=True)):
            with open(path, encoding="utf-8", errors="replace") as fh:
                for line in fh:
                    parts = line.rstrip("\n").split("\t")
                    if len(parts) >= 4:
                        man[parts[0]] = (parts[1], parts[2], parts[3])
    return man


# ⚠ CORES BUILT FROM A FORK OF OUR OWN, whose source is NOT where the recipe points.
# ps4/build-cores.sh keeps the same list in PS4_CORE_FORKS and refuses to overwrite these with an
# upstream build; the recipe still names upstream, so a page generated from the recipe alone would
# point at source that is not what the binary was built from. For a GPL notice that is not a
# cosmetic error - it is the whole claim being wrong.
FORKS = {
    "mednafen_psx_hw": "https://github.com/orbis-ports/beetle-psx-libretro",
}


def read_recipe(path):
    """core -> repository URL, from the recipe the build was driven by."""
    repos = {}
    if not path or not os.path.exists(path):
        repos.update(FORKS)
        return repos
    with open(path, encoding="utf-8", errors="replace") as fh:
        for line in fh:
            if line.startswith("#") or not line.strip():
                continue
            parts = line.split()
            if len(parts) >= 3 and parts[2].startswith("http"):
                repos[parts[0]] = parts[2]
    # ⚠ AFTER the recipe, not before. The recipe still lists upstream for a core we fork, so
    # seeding these first would let it win and the page would name the wrong source.
    repos.update(FORKS)
    return repos


def read_info(info_dir):
    """core -> {display_name, systemname, license}, from the libretro .info bundle."""
    info = {}
    if not info_dir or not os.path.isdir(info_dir):
        return info
    for path in glob.glob(os.path.join(info_dir, "*_libretro.info")):
        name = os.path.basename(path)[: -len("_libretro.info")]
        text = open(path, encoding="utf-8", errors="replace").read()
        fields = {}
        for key in ("display_name", "systemname", "license"):
            m = re.search(r'^%s = "(.*)"' % key, text, re.M)
            if m:
                fields[key] = m.group(1)
        info[name] = fields
    return info


def source_url(repo, sha):
    """A repository URL and a sha, joined the way that host expects."""
    if not repo:
        return None
    repo = repo.rstrip("/")
    if repo.endswith(".git"):
        repo = repo[: -len(".git")]
    if not sha or sha == "-":
        # No sha means the manifest did not record one - a fork built by its own job. The
        # repository link is still correct and still points at source; it is one commit less
        # precise, and saying so beats implying a precision that is not there.
        return repo
    if "github.com" in repo:
        return "%s/tree/%s" % (repo, sha)
    return repo


def human(n):
    for unit in ("B", "KB", "MB"):
        if n < 1024 or unit == "MB":
            return "%.0f %s" % (n, unit) if unit != "B" else "%d B" % n
        n /= 1024.0


def build_bundle(dist, out_path):
    """One archive holding every core, ready to drop on the console over FTP.

    ⚠ The .prx files go in directly rather than the per-core zips. Someone installing without
    console networking wants to unzip once and copy; a zip of zips makes them unpack 101 times.
    """
    members = sorted(glob.glob(os.path.join(dist, "*.prx.zip")))
    if not members:
        return None, 0
    with zipfile.ZipFile(out_path, "w", zipfile.ZIP_DEFLATED) as bundle:
        for path in members:
            with zipfile.ZipFile(path) as inner:
                for entry in inner.infolist():
                    bundle.writestr(entry.filename, inner.read(entry.filename))
    return out_path, len(members)


CSS = """
:root{
  --ground:#12151c; --raised:#1a1f2b; --sunk:#0d1016;
  --ink:#e6e9f0; --muted:#939cad; --faint:#6b7386;
  --rule:#262c3a; --accent:#e96a3a; --warn:#d9a441;
}
*{box-sizing:border-box}
html{-webkit-text-size-adjust:100%}
body{
  margin:0; background:var(--ground); color:var(--ink);
  font:16px/1.65 system-ui,-apple-system,"Segoe UI",Roboto,"Helvetica Neue",Arial,sans-serif;
  padding:0 20px 96px;
}
.wrap{max-width:1100px;margin:0 auto}
/* ⚠ CENTRED, NOT FLUSH LEFT, AND 86 CHARACTERS RATHER THAN 68. The classic 65-75 measure is
   for prose read at length; this is reference material scanned in short passages, and at 68 in
   a 1100px column it looked starved. 86 is past the textbook range on purpose - the line height
   is 1.65, which carries the extra width. The reading column stays bounded because that is what
   reads comfortably - but pinned to the left of a 1100px wrapper it sits under a full-width row of
   download cards with the right half empty, and the page looks broken rather than restrained.
   Centring costs the shared left edge and buys a balanced page; here the wide elements are
   cards and tables, which have their own frames, so the lost alignment is not missed. */
.prose{max-width:86ch;margin-left:auto;margin-right:auto}
code,kbd,.mono{font-family:ui-monospace,SFMono-Regular,Menlo,Consolas,"Liberation Mono",monospace}
code{background:var(--sunk);border:1px solid var(--rule);border-radius:4px;padding:.08em .38em;font-size:.88em}
a{color:var(--accent)}
a:focus-visible,button:focus-visible{outline:2px solid var(--accent);outline-offset:2px}

header{border-bottom:1px solid var(--rule);padding:56px 0 32px;margin-bottom:40px}
/* The masthead is centred; the sections below are not. A masthead is looked at, and centring
   is what that shape expects. A section heading is read, and belongs on the same left edge as
   the cards or table it introduces. */
.brand{display:flex;align-items:center;gap:18px;flex-wrap:wrap;justify-content:center}
.mark{width:56px;height:56px;flex:0 0 auto;border-radius:12px;display:block}
h1{font-size:2rem;margin:0;letter-spacing:-.015em;text-wrap:balance;text-align:center}
.tag{color:var(--muted);margin:.5rem auto 0;max-width:60ch;text-align:center}
.meta{margin-top:20px;display:flex;gap:10px;flex-wrap:wrap;font-size:.82rem;justify-content:center}
.pill{background:var(--raised);border:1px solid var(--rule);border-radius:999px;padding:4px 12px;color:var(--muted)}
.pill b{color:var(--ink);font-weight:600}

h2{font-size:1.28rem;margin:52px 0 14px;letter-spacing:-.01em}
h3{font-size:1rem;margin:28px 0 8px;color:var(--muted);text-transform:uppercase;letter-spacing:.07em}
p{margin:0 0 14px}

.grid{display:grid;gap:14px;grid-template-columns:repeat(auto-fit,minmax(270px,1fr));margin:22px 0}
.card{background:var(--raised);border:1px solid var(--rule);border-radius:10px;padding:20px;display:flex;flex-direction:column;gap:6px}
.card .what{font-weight:600;font-size:1.02rem}
.card .why{color:var(--muted);font-size:.9rem;flex:1}
.card .go{margin-top:10px;display:inline-block;background:var(--accent);color:#12151c;font-weight:650;
  text-decoration:none;padding:9px 15px;border-radius:7px;text-align:center;font-size:.92rem}
.card .size{color:var(--faint);font-size:.8rem;font-variant-numeric:tabular-nums}

ol.steps{margin:0 0 14px;padding-left:1.3em}
ol.steps li{margin:.4em 0}

.note{border-left:3px solid var(--warn);background:var(--raised);padding:14px 18px;border-radius:0 8px 8px 0;margin:20px 0}
/* ⚠ THE BOX IS WIDE; THE MEASURE MUST NOT BE. Directly under the download buttons, this note
   answers the question those buttons raise, so it spans them - but 940px of 16px text is about
   105 characters per line, half again past comfortable. Two columns fill the width and keep
   each line near 60. Justifying instead would only trade a ragged edge for word-space rivers:
   there is no hyphenation to absorb the slack, and unbreakable tokens like CE-34878-0 and
   /data/retroarch/savestates would blow gaps through the lines that carry them. */
.note.wide{max-width:none}
@media (min-width:760px){
  .note.wide{column-count:2;column-gap:34px}
  .note.wide p{margin-top:0;break-inside:avoid}
  .note.wide p:last-child{margin-bottom:0}
}
.note b{color:var(--warn)}

/* ⚠ THE TABLE GETS THE WHOLE COLUMN, AND THE COLUMN IS WIDE ENOUGH FOR IT. An earlier attempt
   pulled the table outside the wrapper with a negative margin, which widened the page's scroll
   area and left every other block sitting off-centre - the fix for one column pushed the entire
   document to the left. The wrapper is 1100px instead, cells wrap, and nothing escapes it. */
.tablewrap{overflow-x:auto;border:1px solid var(--rule);border-radius:10px;margin:18px 0}
table{border-collapse:collapse;width:100%;font-size:.86rem}
th,td{text-align:left;padding:9px 13px;border-bottom:1px solid var(--rule);vertical-align:top}
td:first-child{min-width:22ch}
th:nth-child(3),td:nth-child(3){min-width:11ch}
th{background:var(--sunk);color:var(--muted);font-weight:600;position:sticky;top:0}
tr:last-child td{border-bottom:0}
td.num{font-variant-numeric:tabular-nums;color:var(--muted);white-space:nowrap}
td.mono{white-space:nowrap}
td.sys{color:var(--muted);min-width:12ch}
.nc{display:inline-block;background:rgba(217,164,65,.14);color:var(--warn);border-radius:4px;
  padding:1px 7px;font-size:.78rem;margin-left:6px}

footer{margin-top:64px;padding-top:24px;border-top:1px solid var(--rule);color:var(--faint);font-size:.85rem}
@media (max-width:600px){ header{padding-top:36px} h1{font-size:1.6rem} }
"""

def icon_data_uri(path):
    """The application's own icon0.png, inlined.

    ⚠ THE SAME FILE THE PACKAGE SHIPS, not a drawing of it. An earlier version of this page
    carried a hand-written SVG approximation, which looked like an approximation. The icon is
    already a build input - ps4/icon0.png, 512x512, referenced by Makefile.orbis - so the page
    and the thing on the console's home screen cannot drift apart.

    Inlined rather than linked because this page's whole audience is people about to be offline:
    saved to disk, it still has its icon and needs nothing from the network.
    """
    if not path or not os.path.exists(path):
        return None
    with open(path, "rb") as fh:
        return "data:image/png;base64," + base64.b64encode(fh.read()).decode("ascii")


NONCOMMERCIAL = ("non-commercial", "noncommercial", "mame", "cc by-nc")


def is_noncommercial(lic):
    low = (lic or "").lower()
    return any(k in low for k in NONCOMMERCIAL)


def render(ctx):
    e = html.escape
    rows = []
    nc_count = 0
    for date, crc, filename, name in ctx["index"]:
        info = ctx["info"].get(name, {})
        display = info.get("display_name") or name
        system = info.get("systemname") or ""
        lic = info.get("license") or "—"
        if is_noncommercial(lic):
            nc_count += 1
        result, size, sha = ctx["man"].get(name, ("", "", ""))
        src = source_url(ctx["repos"].get(name), sha)
        src_cell = ('<a href="%s">%s</a>' % (e(src), e(sha)) if src and sha and sha != "-"
                    else (('<a href="%s">source</a>' % e(src)) if src else "—"))
        rows.append(
            "<tr><td><a href=\"%s%s\">%s</a></td><td class=\"sys\">%s</td>"
            "<td>%s%s</td><td class=\"num\">%s</td><td class=\"mono num\">%s</td></tr>"
            % (e(ctx["base"]), e(filename), e(display), e(system),
               e(lic), '<span class="nc">non-commercial</span>' if is_noncommercial(lic) else "",
               e(size), src_cell))

    bundle_row = ""
    if ctx.get("bundle_name"):
        bundle_row = (
            '<div class="card"><span class="what">Every core, one archive</span>'
            '<span class="why">All %d cores as <code>.prx</code> files. For a console with no '
            'network: unzip and copy them across over FTP.</span>'
            '<span class="size">%s</span>'
            '<a class="go" href="%s%s">Download cores</a></div>'
            % (len(ctx["index"]), e(ctx["bundle_size"]), e(ctx["base"]), e(ctx["bundle_name"])))

    return """<!doctype html>
<html lang="en"><head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
%s
<title>RetroArchV for PlayStation 4</title>
<meta name="description" content="RetroArch for the PlayStation 4: the package, %d cores, and the source they were built from.">
<style>%s</style>
</head><body><div class="wrap">

<header>
  <div class="brand">%s<div>
    <h1>RetroArchV for PlayStation 4</h1>
    <p class="tag">Emulation on a jailbroken console, running Vulkan through RADV and
       OpenGL&nbsp;ES through zink. %d cores, built for this hardware.</p>
  </div></div>
  <div class="meta">
    <span class="pill">Package <b>%s</b></span>
    <span class="pill">Cores <b>%d</b></span>
    <span class="pill">Index <b>%s</b></span>
    <span class="pill">Firmware <b>%s</b></span>
    <span class="pill">GoldHEN <b>%s</b></span>
  </div>
</header>

<h2>Download</h2>
<div class="grid">
  <div class="card">
    <span class="what">RetroArchV %s</span>
    <span class="why">The application package. Install it once; everything else can come later
      over the console's own updater.</span>
    <span class="size">%s</span>
    <a class="go" href="%s">Download .pkg</a>
  </div>
  %s
</div>

<div class="note wide">
<p><b>Quit from inside the application, not from the console.</b> Use RetroArch's own
<b>Quit RetroArch</b> entry, or the Quit combo on the pad. It returns to the console's menu with
no dialog. Earlier versions showed <code>CE-34878-0</code> here; they ended by returning from
<code>main()</code>, which takes the process down outside the path the system expects.</p>
<p>Closing it the console's way instead — the PS button, then <em>Close Application</em> — still
shows <code>CE-34878-0</code>. The console returns to its menu and nothing needs restarting, but the
application is killed outright there and never gets to shut itself down, so Quit is the better
habit.</p>
</div>

<div class="prose">
<h2>Install</h2>
<ol class="steps">
  <li>Copy the <code>.pkg</code> to <code>/data/pkg</code> on the console over FTP.</li>
  <li>On the console: <b>Settings &rarr; Debug Settings &rarr; Package Installer</b>.</li>
  <li>Pick the package and install it.</li>
  <li>Launch <b>RetroArchV</b>.</li>
</ol>
<p><b>Tested on firmware %s with GoldHEN %s</b>, by SiSTRo. Other firmware and other jailbreak
builds may work — nobody has checked, and a report either way is useful.</p>
<p>It installs under its own title id, <code>RTRV00001</code>, so it sits <em>beside</em> any
RetroArch already on the console rather than replacing it — the system decides collisions by
title id, not by the name on screen.</p>

<h3>First run</h3>
<p><b>Online Updater &rarr; Update Core Info Files.</b> Do this before browsing the core list.
Core metadata is not shipped inside the package, so until those files arrive the downloader
lists <code>.prx</code> filenames instead of names like &ldquo;Nintendo&nbsp;64&nbsp;(Mupen64Plus-Next)&rdquo;.
Everything works either way; only the labels are missing.</p>

<h2>With a console that has no network</h2>
<p>Download the archive above on another machine, unzip it, and copy the <code>.prx</code> files
into <code>/data/retroarch/cores</code> over FTP. Nothing else is needed — the core list is read
from that directory at startup.</p>
<div class="note">
<p><b>Copy them, do not extract them on the console.</b> A file arriving over FTP is created
executable, which is what the module loader requires. Files unpacked by other routes may not be,
and a core without that bit fails to load with no explanation beyond
&ldquo;Failed to open libretro core&rdquo;.</p>
</div>
<p>Core metadata works the same way offline: the <code>.info</code> files live in
<code>/data/retroarch/info</code> and can be copied across from
<a href="http://buildbot.libretro.com/assets/frontend/info.zip">libretro's bundle</a>.</p>

<h2>Where everything lives</h2>
<p>All of it sits under <code>/data/retroarch/</code>, which is writable and survives
reinstalling the package. Put files there over FTP and RetroArch finds them at startup — there is
no import step.</p>
</div>

<div class="tablewrap"><table>
<thead><tr><th>Directory</th><th>What goes in it</th></tr></thead>
<tbody>
<tr><td class="mono">/data/retroarch/cores</td><td class="sys">Cores, as <code>.prx</code> files</td></tr>
<tr><td class="mono">/data/retroarch/info</td><td class="sys">Core metadata — names, supported extensions, required BIOS files</td></tr>
<tr><td class="mono">/data/retroarch/system</td><td class="sys">BIOS and firmware. A core that needs one looks here and nowhere else</td></tr>
<tr><td class="mono">/data/retroarch/roms</td><td class="sys">Content. Only a convention — games can sit anywhere the console can read</td></tr>
<tr><td class="mono">/data/retroarch/savefiles</td><td class="sys">Battery saves, memory cards</td></tr>
<tr><td class="mono">/data/retroarch/savestates</td><td class="sys">Save states</td></tr>
<tr><td class="mono">/data/retroarch/config</td><td class="sys">Per-core configuration overrides, and <code>config/remaps</code> for input remaps</td></tr>
<tr><td class="mono">/data/retroarch/playlists</td><td class="sys">Playlists built by the scanner</td></tr>
<tr><td class="mono">/data/retroarch/thumbnails</td><td class="sys">Box art and screenshots</td></tr>
<tr><td class="mono">/data/retroarch/shaders</td><td class="sys">Shader presets and passes</td></tr>
<tr><td class="mono">/data/retroarch/overlays</td><td class="sys">Overlays, and <code>overlays/keyboards</code> for on-screen keyboards</td></tr>
<tr><td class="mono">/data/retroarch/assets</td><td class="sys">Menu assets — icons, fonts, the XMB and Ozone themes</td></tr>
<tr><td class="mono">/data/retroarch/database/rdb</td><td class="sys">Content databases the scanner matches against</td></tr>
<tr><td class="mono">/data/retroarch/cheats</td><td class="sys">Cheat files</td></tr>
<tr><td class="mono">/data/retroarch/logs</td><td class="sys">Logs</td></tr>
<tr><td class="mono">/data/retroarch/retroarch.cfg</td><td class="sys">The main configuration file</td></tr>
</tbody></table></div>

<div class="prose">
<div class="note">
<p><b>Edit the configuration with RetroArch closed.</b> It keeps its settings in memory and
writes the whole file out when it exits, so a change made over FTP while it is running is
overwritten on quit — not merged, replaced.</p>
</div>
<p>The package itself is mounted read-only at <code>/app0</code> and holds only the executable,
the icon and the certificate bundle. Nothing there needs editing, and nothing there can be.</p>

<h2>Configuring it</h2>
<p>Once the cores are in place, this is ordinary RetroArch. Controller mapping, shaders,
overlays, per-core options, save states, rewind, netplay's absence, the scanner, playlists — all
of it behaves as it does everywhere else, with the same menus in the same places. The
<a href="https://docs.libretro.com/">libretro documentation</a> applies unchanged, and so does
any guide written for another platform.</p>
<p>Two habits worth having early: <b>Load Content</b> scans a directory for anything a core
claims, and per-core settings live under <b>Quick Menu &rarr; Options</b> while the game is
running, saved with <b>Manage Core Options</b>.</p>

<h2>Source code and licences</h2>
<p>RetroArch is licensed under the <b>GNU General Public License, version 3</b>, and most cores
here are GPL of one vintage or another. Distributing these binaries obliges us to hand you the
source they were built from — not the project in general, but the exact commit. That is what the
last column of the table below is: each core's version links to the tree it was compiled from.</p>
<p>The frontend and the platform work:</p>
<ul>
  <li><a href="%s">orbis-ports/RetroArch</a> at <code>%s</code> — the frontend, GPLv3, forked from
      <a href="https://github.com/libretro/RetroArch">libretro/RetroArch</a></li>
  <li><a href="https://github.com/orbis-ports/mesa-ps4">orbis-ports/mesa-ps4</a> — Mesa with a
      PlayStation&nbsp;4 winsys and video-out layer (MIT)</li>
  <li><a href="https://github.com/orbis-ports/orbis-compat">orbis-ports/orbis-compat</a> — the
      platform overlay: libc and pthread corrections, packaging, logging</li>
  <li><a href="https://github.com/orbis-ports/beetle-psx-libretro">orbis-ports/beetle-psx-libretro</a>
      — the PlayStation core, forked for this platform</li>
  <li><a href="https://bearssl.org/">BearSSL</a> (MIT) for TLS, and the
      <a href="https://github.com/OpenOrbis/OpenOrbis-PS4-Toolchain">OpenOrbis toolchain</a></li>
</ul>
<div class="note">
<p><b>%d of these cores carry non-commercial terms</b> and are marked in the table. They may not
be redistributed commercially, and neither may this collection as a whole while they are part of
it. Nothing here is sold, and nothing here should be.</p>
</div>

<h2>Known limits</h2>
<ul>
  <li>Closing the application from the console's own menu still shows <code>CE-34878-0</code>.
      It is cosmetic — the console recovers on its own. Quit from inside RetroArch does not.</li>
  <li>63 of the 164 cores in the build recipe do not compile for this platform yet. The ones
      listed here are the ones that link and export a working entry point.</li>
  <li>Cores are built from upstream's tip on the day the build ran, so a core's version is a
      date rather than a release number.</li>
</ul>
</div>

<h2>The cores</h2>
<p class="prose">Every file the console's own Core Downloader offers, and where each came from.</p>
<div class="tablewrap"><table>
<thead><tr><th>Core</th><th>System</th><th>Licence</th><th>Size</th><th>Built from</th></tr></thead>
<tbody>
%s
</tbody></table></div>

<footer>
  <p>Index published %s &middot; served from <code>%s</code> &middot;
     this page is generated by <code>ps4/make-site.py</code> in the frontend repository,
     from the same manifests as the binaries it describes.</p>
</footer>

</div></body></html>
""" % (ctx["favicon"], len(ctx["index"]), CSS, ctx["mark"], len(ctx["index"]),
       e(ctx["version"]), len(ctx["index"]), e(ctx["index_date"]),
       e(ctx["firmware"]), e(ctx["goldhen"]),
       e(ctx["version"]), e(ctx["pkg_size"]), e(ctx["pkg_url"]),
       bundle_row,
       e(ctx["firmware"]), e(ctx["goldhen"]),
       e(ctx["src_url"]), e(ctx["version"]),
       nc_count,
       "\n".join(rows),
       e(ctx["index_date"]), e(ctx["base"]))


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--index", required=True, help=".index-extended the page describes")
    ap.add_argument("--manifests", action="append", default=[],
                    help="directory holding cores-manifest-*.tsv; repeatable")
    ap.add_argument("--recipe", help="the libretro-super recipe the build was driven by")
    ap.add_argument("--info", help="directory of unpacked .info files")
    ap.add_argument("--icon", help="the application icon, inlined into the page")
    ap.add_argument("--firmware", default="11.00", help="console firmware this was tested on")
    ap.add_argument("--goldhen", default="2.4b18.10", help="GoldHEN build this was tested on")
    ap.add_argument("--dist", help="directory of *.prx.zip, for --bundle")
    ap.add_argument("--out", required=True, help="where index.html and the bundle are written")
    ap.add_argument("--base", required=True, help="public base URL, with trailing slash")
    ap.add_argument("--pkg-url", required=True, help="link for the .pkg")
    ap.add_argument("--pkg-size", default="", help="human-readable size shown on the button")
    ap.add_argument("--version", required=True, help="e.g. v0.1.2")
    ap.add_argument("--src-url", required=True, help="frontend source at this version")
    ap.add_argument("--bundle", action="store_true", help="also build the all-cores archive")
    ap.add_argument("--bundle-name", help="link an archive that already exists, without rebuilding")
    ap.add_argument("--bundle-size", default="", help="its size, for the button")
    args = ap.parse_args()

    base = args.base if args.base.endswith("/") else args.base + "/"
    os.makedirs(args.out, exist_ok=True)

    index = read_index(args.index)
    if not index:
        sys.stderr.write("!! %s lists no cores - refusing to write a page about nothing\n"
                         % args.index)
        return 1

    ctx = {
        "index": index,
        "man": read_manifests(args.manifests),
        "repos": read_recipe(args.recipe),
        "info": read_info(args.info),
        "base": base,
        "version": args.version,
        "pkg_url": args.pkg_url,
        "pkg_size": args.pkg_size,
        "src_url": args.src_url,
        "index_date": index[0][0],
        "bundle_name": None,
        "bundle_size": "",
        "firmware": args.firmware,
        "goldhen": args.goldhen,
    }

    icon = icon_data_uri(args.icon)
    ctx["mark"] = ('<img class="mark" src="%s" alt="" width="56" height="56">' % icon) if icon else ""
    ctx["favicon"] = ('<link rel="icon" href="%s">' % icon) if icon else ""

    if not args.bundle and not args.bundle_name:
        # ⚠ SAY SO. Without --bundle the page renders perfectly well and simply has no
        # "every core, one archive" card - which is the single thing an offline visitor came
        # for. It went missing exactly once that way, silently, on a page that looked fine.
        print("== NO BUNDLE: the page will have no all-cores download. Pass --bundle --dist "
              "<dir>, or --bundle-name <file> to point at one that already exists.",
              file=sys.stderr)

    if args.bundle_name and not args.bundle:
        ctx["bundle_name"] = args.bundle_name
        ctx["bundle_size"] = args.bundle_size

    if args.bundle:
        if not args.dist:
            sys.stderr.write("!! --bundle needs --dist\n")
            return 1
        name = "orbis-cores-%s.zip" % index[0][0]
        path, count = build_bundle(args.dist, os.path.join(args.out, name))
        if not path:
            sys.stderr.write("!! no *.prx.zip in %s\n" % args.dist)
            return 1
        ctx["bundle_name"] = name
        ctx["bundle_size"] = human(os.path.getsize(path))
        print("== bundle: %s, %d cores, %s" % (name, count, ctx["bundle_size"]))

    # ⚠ Say what is missing rather than quietly rendering a dash. A core with no sha cannot have
    # its corresponding source pointed at, which is the one thing this page exists to do.
    no_sha = [n for _, _, _, n in index
              if not ctx["man"].get(n, ("", "", ""))[2].strip("-")]
    no_repo = [n for _, _, _, n in index if not ctx["repos"].get(n)]
    no_lic = [n for _, _, _, n in index if not ctx["info"].get(n, {}).get("license")]
    for label, names in (("no commit sha", no_sha), ("no repository", no_repo),
                         ("no licence", no_lic)):
        if names:
            print("== %d core(s) with %s: %s" % (len(names), label, " ".join(sorted(names))))

    out_html = os.path.join(args.out, "index.html")
    with open(out_html, "w", encoding="utf-8") as fh:
        fh.write(render(ctx))
    print("== %s, %d cores, %d bytes" % (out_html, len(index), os.path.getsize(out_html)))
    return 0


if __name__ == "__main__":
    sys.exit(main())
