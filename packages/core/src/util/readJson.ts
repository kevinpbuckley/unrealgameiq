import { readFileSync } from "node:fs";

/**
 * Read + parse a JSON file written by any producer, tolerating encoding quirks.
 *
 * The UE plugin writes via FFileHelper::SaveStringToFile, which auto-detects
 * encoding and emits UTF-16LE-with-BOM when the content has non-ASCII characters
 * (e.g. `→`/`…` in Blueprint pseudocode). Node's default utf8 read mangles that,
 * so we sniff the BOM and decode accordingly. Being liberal here keeps the
 * cross-language ExtractorOutput contract robust.
 */
export function readJsonFile(path: string): unknown {
  const buf = readFileSync(path);
  let text: string;
  if (buf.length >= 2 && buf[0] === 0xff && buf[1] === 0xfe) {
    text = buf.subarray(2).toString("utf16le");
  } else if (buf.length >= 2 && buf[0] === 0xfe && buf[1] === 0xff) {
    // UTF-16BE: swap byte pairs, then decode as LE
    const swapped = Buffer.from(buf.subarray(2));
    swapped.swap16();
    text = swapped.toString("utf16le");
  } else if (buf.length >= 3 && buf[0] === 0xef && buf[1] === 0xbb && buf[2] === 0xbf) {
    text = buf.subarray(3).toString("utf8");
  } else {
    text = buf.toString("utf8");
  }
  if (text.charCodeAt(0) === 0xfeff) text = text.slice(1); // stray BOM char
  return JSON.parse(text);
}
