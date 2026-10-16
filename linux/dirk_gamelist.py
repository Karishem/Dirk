#!/usr/bin/env python3
# Dirk: adds each game's frontend media (the Media/ folder the Dirk
# Downloader fills) to EmulationStation's gamelist.xml, so the theme shows
# the game's logo, box, disc, background and video.
#
#   python3 dirk_gamelist.py <gamelist.xml> <list.tsv> [<system folder>]
#
# list.tsv, one line per entry, tab separated:
#   game    <path of the game for ES>   <display name>   <game folder>
#   folder  <path of the folder for ES> <display name>   <game folder>
# A path starting with "./" is relative to the system's rom folder (as ES
# reads them; by default the gamelist's folder); media paths are written
# the same way when the game folder is under it, else as absolute paths.
#
# Media/ file names (Cyril's, same for every game, any case):
#   clear.png      logo                -> marquee, wheel
#   3D.png         3D box              -> thumbnail, boxart
#   snap.mp4       the game's video    -> video (else any video file)
#   laserdisc.png  the disc            -> cartridge
#   background.png background          -> image (the theme's main
#                                         picture) and fanart
#   marquee.png/.jpg arcade marquee    -> marquee when there is no clear.png
#
# Rules: nothing else in the file is touched (other games, names, ratings,
# play counts...). A media tag is written when it is missing or when it
# already points into the game's own Media/ folder; a tag pointing anywhere
# else (an image the player chose or scraped) is kept. New games get an
# entry with their name. Written only when something changed; the previous
# file is kept as gamelist.xml.bak.
# Python 3.5+ (ArkOS, Batocera).

import os
import sys
import xml.etree.ElementTree as ET

VIDEO_EXT = ('.mp4', '.mkv', '.avi', '.webm', '.mov', '.m4v')


def media_files(game_dir):
    """{lower-case name: real name} of game_dir/Media (any case of 'Media')."""
    for d in os.listdir(game_dir) if os.path.isdir(game_dir) else []:
        p = os.path.join(game_dir, d)
        if d.lower() == 'media' and os.path.isdir(p):
            return p, dict((f.lower(), f) for f in sorted(os.listdir(p))
                           if os.path.isfile(os.path.join(p, f)))
    return None, {}


def wanted_media(game_dir):
    """[(tag, absolute file)] for this game, in ES order."""
    mdir, files = media_files(game_dir)
    if mdir is None:
        return [], None

    def f(*names):
        for n in names:
            if n in files:
                return os.path.join(mdir, files[n])
        return None

    video = None
    for low in sorted(files):
        if low.endswith(VIDEO_EXT):
            if low.startswith('snap.') or video is None:
                video = os.path.join(mdir, files[low])
    out = [('image', f('background.png', 'background.jpg', '3d.png')),
           ('thumbnail', f('3d.png')),
           ('marquee', f('clear.png', 'marquee.png', 'marquee.jpg')),
           ('wheel', f('clear.png')),
           ('boxart', f('3d.png')),
           ('cartridge', f('laserdisc.png')),
           ('fanart', f('background.png', 'background.jpg')),
           ('video', video)]
    return [(t, p) for t, p in out if p], mdir


def es_path(abs_path, base):
    rel = os.path.relpath(abs_path, base)
    if rel == '..' or rel.startswith('..' + os.sep):
        return abs_path
    return './' + rel.replace(os.sep, '/')


def resolve(es, base):
    if es.startswith('./'):
        return os.path.normpath(os.path.join(base, es[2:]))
    return os.path.normpath(es)


def in_media(abs_file, game_dir):
    """True if abs_file is inside game_dir/Media (any case)."""
    rel = os.path.relpath(abs_file, os.path.normpath(game_dir))
    parts = rel.split(os.sep)
    return len(parts) >= 2 and parts[0].lower() == 'media'


def indent(e, level=0):
    pad = '\n' + '\t' * level
    if len(e):
        if not (e.text or '').strip():
            e.text = pad + '\t'
        for c in e:
            indent(c, level + 1)
        if not (c.tail or '').strip():
            c.tail = pad
    if level and not (e.tail or '').strip():
        e.tail = pad


def main():
    if len(sys.argv) not in (3, 4):
        sys.stderr.write('usage: dirk_gamelist.py <gamelist.xml> <list.tsv> [<system folder>]\n')
        return 2
    gamelist, listfile = sys.argv[1], sys.argv[2]
    base = os.path.abspath(sys.argv[3] if len(sys.argv) == 4
                           else os.path.dirname(os.path.abspath(gamelist)))

    if os.path.isfile(gamelist) and os.path.getsize(gamelist) > 0:
        try:
            tree = ET.parse(gamelist)
        except ET.ParseError as e:
            print('gamelist.xml unreadable ({}), left as is'.format(e))
            return 1
        root = tree.getroot()
    else:
        root = ET.Element('gameList')
        tree = ET.ElementTree(root)
    changed = False

    index = {}
    for el in root:
        p = el.findtext('path')
        if el.tag in ('game', 'folder') and p:
            index[(el.tag, resolve(p.strip(), base))] = el

    with_media = 0
    added = 0
    with open(listfile, encoding='utf-8') as fh:
        for line in fh:
            parts = line.rstrip('\n').split('\t')
            if len(parts) != 4 or parts[0] not in ('game', 'folder'):
                continue
            kind, path, name, game_dir = parts
            media, mdir = wanted_media(game_dir)
            el = index.get((kind, resolve(path, base)))
            if el is None:
                if not media and kind == 'folder':
                    continue          # nothing to say about a bare folder
                el = ET.SubElement(root, kind)
                ET.SubElement(el, 'path').text = path
                ET.SubElement(el, 'name').text = name
                index[(kind, resolve(path, base))] = el
                added += 1
                changed = True
            if media:
                with_media += 1
            want = dict(media)
            for tag in ('image', 'thumbnail', 'marquee', 'wheel', 'boxart',
                        'cartridge', 'fanart', 'video'):
                cur = el.find(tag)
                cur_abs = resolve(cur.text.strip(), base) if cur is not None and cur.text else None
                mine = cur_abs is not None and in_media(cur_abs, game_dir)
                if tag in want:
                    new = es_path(want[tag], base)
                    if cur is None:
                        ET.SubElement(el, tag).text = new
                        changed = True
                    elif (mine or not cur.text) and cur.text != new:
                        cur.text = new
                        changed = True
                elif cur is not None and mine and not os.path.isfile(cur_abs):
                    el.remove(cur)    # our file was removed from Media/
                    changed = True

    if not changed and os.path.isfile(gamelist):
        print('gamelist.xml: up to date ({} with media)'.format(with_media))
        return 0
    if os.path.isfile(gamelist):
        try:
            with open(gamelist, 'rb') as a, open(gamelist + '.bak', 'wb') as b:
                b.write(a.read())
        except OSError:
            pass
    indent(root)
    tmp = gamelist + '.tmp'
    tree.write(tmp, encoding='utf-8', xml_declaration=True)
    os.replace(tmp, gamelist)
    print('gamelist.xml: {} with media, {} new entr{}'.format(
        with_media, added, 'y' if added == 1 else 'ies'))
    return 0


if __name__ == '__main__':
    sys.exit(main())
