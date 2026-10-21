/* Live mobile chat acceptance against an ADB-forwarded QZdesk web UI.
 *
 * Run from Windows Node so Playwright can launch Edge:
 *   node tools/check_live_chat.cjs
 *
 * Add --send to submit one real message and wait for the SSE reply. Add
 * --abort to submit a second message and exercise the stop button.
 */
const fs = require('node:fs');
const path = require('node:path');
const assert = require('node:assert/strict');

const playwrightPath = process.env.QZ_PLAYWRIGHT_PATH
    || '//wsl.localhost/Ubuntu-22.04/home/jn/.npm/_npx/705bc6b22212b352/node_modules/playwright';
const { chromium } = require(playwrightPath);

const url = process.env.QZ_CHAT_URL || 'http://127.0.0.1:18080/#chat';
const edge = process.env.QZ_EDGE_PATH
    || 'C:/Program Files (x86)/Microsoft/Edge/Application/msedge.exe';
const output = path.resolve(process.env.QZ_CHAT_OUTPUT || path.join(__dirname, '../build/web-live-chat'));
const shouldSend = process.argv.includes('--send');
const shouldAbort = process.argv.includes('--abort');
const prompt = process.env.QZ_CHAT_PROMPT
    || '这是手机输入验收，请只回复一句：手机输入正常。';

function viewportBounds(page) {
    return page.evaluate(() => {
        const card = document.querySelector('.compact-active.chat-card');
        const input = document.querySelector('#chatInput');
        const send = document.querySelector('#chatSend');
        const back = document.querySelector('#chatBack');
        const appbar = document.querySelector('.appbar');
        const rect = card.getBoundingClientRect();
        const inputRect = input.getBoundingClientRect();
        const sendRect = send.getBoundingClientRect();
        const backRect = back.getBoundingClientRect();
        return {
            width: innerWidth,
            height: innerHeight,
            rootWidth: document.documentElement.scrollWidth,
            rootHeight: document.documentElement.scrollHeight,
            card: { left: rect.left, top: rect.top, right: rect.right, bottom: rect.bottom },
            input: { left: inputRect.left, top: inputRect.top, right: inputRect.right, bottom: inputRect.bottom },
            send: { left: sendRect.left, top: sendRect.top, right: sendRect.right, bottom: sendRect.bottom },
            back: { left: backRect.left, top: backRect.top, right: backRect.right, bottom: backRect.bottom },
            appbarHidden: getComputedStyle(appbar).display === 'none',
            backVisible: getComputedStyle(back).display !== 'none',
            sendIcon: (() => {
                const icon = send.querySelector('svg').getBoundingClientRect();
                return { width: icon.width, height: icon.height };
            })(),
            turns: document.querySelectorAll('#chatMessages .chat-turn').length,
            authors: document.querySelectorAll('#chatMessages .chat-author').length,
            active: document.activeElement === input,
            focusVisible: input.matches(':focus')
        };
    });
}

(async () => {
    fs.mkdirSync(output, { recursive: true });
    const browser = await chromium.launch({ headless: true, executablePath: edge });
    const context = await browser.newContext({
        viewport: { width: 360, height: 640 },
        screen: { width: 360, height: 640 },
        deviceScaleFactor: 1,
        isMobile: true,
        hasTouch: true,
        reducedMotion: 'reduce'
    });
    const page = await context.newPage();
    const errors = [];
    page.on('pageerror', error => errors.push(error.message));

    try {
        await page.goto(url, { waitUntil: 'domcontentloaded', timeout: 30000 });
        await page.waitForSelector('.compact-active.chat-card', { timeout: 15000 });
        await page.locator('#chatInput').tap();
        await page.locator('#chatInput').fill(prompt);
        const focused = await viewportBounds(page);
        assert.equal(focused.active, true, 'chat input receives focus');
        assert.equal(focused.focusVisible, true, 'chat input shows focus');
        assert.equal(focused.appbarHidden, true, 'console navigation is not duplicated in chat');
        assert.equal(focused.backVisible, true, 'chat keeps a return control');
        assert.ok(focused.rootWidth <= focused.width, 'no horizontal page overflow');
        assert.ok(focused.rootHeight <= focused.height + 1, 'no vertical page overflow');
        for (const [name, rect] of Object.entries(focused.card ? { card: focused.card } : {})) {
            assert.ok(rect.left >= 0 && rect.top >= 0 && rect.right <= focused.width &&
                rect.bottom <= focused.height, `${name} stays in viewport`);
        }
        for (const [name, rect] of Object.entries({
            input: focused.input, send: focused.send, back: focused.back
        })) {
            assert.ok(rect.left >= 0 && rect.top >= 0 && rect.right <= focused.width &&
                rect.bottom <= focused.height, `${name} stays in viewport`);
        }
        assert.ok(focused.sendIcon.width >= 22 && focused.sendIcon.height >= 22,
            'send icon is visually legible');
        assert.ok(focused.turns > 0 && focused.authors > 0,
            'messages expose sender identity');
        await page.screenshot({ path: path.join(output, 'mobile-focused.png') });

        const report = { url, viewport: [360, 640], focused, sent: false, reply: false, aborted: false };

        if (shouldSend) {
            const before = await page.locator('#chatMessages .chat-bubble').count();
            const [response] = await Promise.all([
                page.waitForResponse(resp => resp.url().endsWith('/api/chat/send'), { timeout: 15000 }),
                page.locator('#chatForm').evaluate(form => form.requestSubmit())
            ]);
            assert.equal(response.status(), 202, 'chat send accepted by core');
            const accepted = await response.json();
            assert.equal(accepted.ok, true, 'chat send response ok');
            assert.equal(accepted.accepted, true, 'chat send accepted flag');
            report.sent = true;
            await page.waitForFunction(
                count => document.querySelectorAll('#chatMessages .chat-bubble').length >= count + 2,
                before,
                { timeout: 180000 }
            );
            await page.waitForFunction(
                () => !document.querySelector('#chatState')?.textContent.includes('处理中')
                    && !document.querySelector('#chatState')?.textContent.includes('等待回复'),
                { timeout: 180000 }
            );
            report.reply = true;
            report.state = (await page.locator('#chatState').textContent()).trim();
            report.lastMessage = (await page.locator('#chatMessages .chat-bubble').last().textContent()).trim();
            await page.screenshot({ path: path.join(output, 'mobile-replied.png') });
        }

        if (shouldAbort) {
            await page.locator('#chatInput').fill('请慢慢数到一百，用于验证停止按钮。');
            const [response] = await Promise.all([
                page.waitForResponse(resp => resp.url().endsWith('/api/chat/send'), { timeout: 15000 }),
                page.locator('#chatForm').evaluate(form => form.requestSubmit())
            ]);
            assert.equal(response.status(), 202, 'abort test message accepted');
            await page.waitForFunction(
                () => {
                    const button = document.querySelector('#chatAbort');
                    return button && !button.disabled && getComputedStyle(button).display !== 'none';
                },
                { timeout: 15000 }
            );
            const [abortResponse] = await Promise.all([
                page.waitForResponse(resp => resp.url().endsWith('/api/chat/abort'), { timeout: 15000 }),
                page.locator('#chatAbort').click()
            ]);
            assert.equal(abortResponse.status(), 200, 'chat abort accepted');
            const aborted = await abortResponse.json();
            assert.equal(aborted.ok, true, 'chat abort response ok');
            report.aborted = true;
            await page.screenshot({ path: path.join(output, 'mobile-aborted.png') });
        }

        assert.deepEqual(errors, [], 'browser runtime errors');
        console.log(JSON.stringify(report, null, 2));
        console.log(`Screenshots: ${output}`);
    } finally {
        await context.close();
        await browser.close();
    }
})().catch(error => {
    console.error(error);
    process.exitCode = 1;
});
