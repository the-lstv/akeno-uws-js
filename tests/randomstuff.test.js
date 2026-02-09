/**
 * This is a testing utility that just does whatever and tries some edge cases.
 */

const uws = require("../dist/uws");

const app = new uws.App();

class Extends extends uws.HTTPProtocol {
    constructor() {
        super();
        console.log("Extending works");
    }
}

// Just testing that we can safely extend HTTPProtocol (Akeno currently relies on this for legacy JS APIs)
const http = new Extends();

const parser = new uws.HTMLParser({
    buffer: true,
    onText: () => {},
});

console.log("HTMLParser:", parser);

// Test parsing from a string (markdown)
console.log("Parsing from a string (markdown): ", parser.fromMarkdownString("# Hello World\n", parser.createContext()).toString());

// Test parsing from a file (html)
console.log("HTML Parsing from a file: ", parser.fromFile(__dirname + "/test.html", parser.createContext()).toString().slice(0, 100).replaceAll("\n", "").replaceAll("\r", "") + "...");

const p = 8089;
http.listen(p, (socket) => console.log(socket)).bind(app);

let tspmo = [];
const baseUrl = "http://127.0.0.1:" + p;

// Test utility function
function test(testString, handler, expected) {
    return tspmo.push(new Promise(async (resolve, reject) => {
        const [hostPart, commentPart] = testString.split("#").map((s) => s.trim());
        const { hostPattern, realHosts } = parseHostPart(hostPart);
        const comment = commentPart || hostPattern;

        app.route(hostPattern, typeof handler === "function" ? handler(expected) : handler);

        try {
            for (const realHost of realHosts) {
                const res = await fetch(baseUrl, { headers: { Host: realHost } }); // This doesn't actually work idk i will check tomorow nowI just want to sleep
                const text = await res.text();

                if (typeof expected === "function") {
                    expected(res, text, realHost, hostPattern);
                } else if (text !== expected) {
                    throw new Error(`Expected ${expected} but got ${text}`);
                }

                console.log(
                    `${comment} PASSED ${hostPattern} (${realHost}) => ${text.slice(0, 100)}${text.length > 100 ? "..." : ""}`
                );
            }

            resolve();
        } catch (err) {
            reject(err);
        }
    }));
}

function parseHostPart(hostPart) {
    const trimmed = hostPart.trim();
    const parenIndex = trimmed.indexOf("(");

    if (parenIndex === -1 || !trimmed.endsWith(")")) {
        return { hostPattern: trimmed, realHosts: [trimmed] };
    }

    const hostPattern = trimmed.slice(0, parenIndex).trim();
    const inner = trimmed.slice(parenIndex + 1, -1);
    const realHosts = inner
        .split(",")
        .map((host) => host.trim())
        .filter(Boolean);

    return { hostPattern, realHosts: realHosts.length ? realHosts : [hostPattern] };
}

let ic = 0;
Object.defineProperty(globalThis, "id", {
    get() {
        return ic++;
    }
});





// -- Begin tests --


test(`${id}.localhost # Direct response`, (v) => ((r, q) => { q.end(v) }), "Hello Akeno!");
test(`${id}.localhost # Write in chunks`, (v) => ((r, q) => { q.write(v.slice(0, 5)); q.write(v.slice(5)); q.end() }), "Hello Akeno!");
test(`${id}.localhost # 404 response`, null, (res) => res.status === 404);
test(`*.localhost (${id}.localhost, ${id}.localhost) # Wildcard with multiple real hosts`, (v) => ((r, q) => { q.end(v) }), "Hello Akeno!");

Promise.all(tspmo).then(() => {
    console.log("All tests passed!");
}).catch((err) => {
    console.error("Test failed:", err);
});