// downloads.pause/resume/open/removeFile/getFileIcon from the background
// page; the MCP evaluate tool calls these. serve.py (beside this file)
// sends /slow.bin at about 1 MB a second with byte ranges and an ETag.
globalThis.events = [];
browser.downloads.onChanged.addListener((delta) => events.push(delta));

const wait = (ms) => new Promise((resolve) => setTimeout(resolve, ms));
const item = async (id) => (await browser.downloads.search({ id }))[0];

globalThis.checks = {
    start: (url = "http://127.0.0.1:8798/slow.bin") => browser.downloads.download({ url, filename: "summit-downloads-control.bin" }),
    item,
    pause: (id) => browser.downloads.pause(id),
    resume: (id) => browser.downloads.resume(id),
    cancel: (id) => browser.downloads.cancel(id),
    open: (id) => browser.downloads.open(id),
    removeFile: (id) => browser.downloads.removeFile(id),
    acceptDanger: (id) => browser.downloads.acceptDanger(id).then(() => "accepted", (error) => error.message),
    icon: (id, size) => browser.downloads.getFileIcon(id, size ? { size } : {}),
    // Start, pause after a second, check it stays paused, resume and wait
    // for the end: what each step saw.
    async pauseAndResume() {
        const steps = [];
        const id = await checks.start();
        await wait(1500);
        await browser.downloads.pause(id);
        let state = await item(id);
        steps.push({ step: "paused", state: state.state, paused: state.paused, canResume: state.canResume, bytes: state.bytesReceived });
        await wait(1500);
        const later = await item(id);
        steps.push({ step: "still paused", bytes: later.bytesReceived, grew: later.bytesReceived > state.bytesReceived });
        await browser.downloads.resume(id);
        state = await item(id);
        steps.push({ step: "resumed", state: state.state, paused: state.paused });
        for (let i = 0; i < 60 && (await item(id)).state === "in_progress"; i++)
            await wait(500);
        state = await item(id);
        steps.push({ step: "end", state: state.state, fileSize: state.fileSize, totalBytes: state.totalBytes, exists: state.exists, filename: state.filename });
        return { id, steps };
    },
};
