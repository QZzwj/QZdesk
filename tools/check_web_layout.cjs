/* Run with Node and Playwright installed: node tools/check_web_layout.cjs.
 * Routes are local fixtures; this never contacts or changes a device. */
const { chromium } = require('playwright');
const fs = require('node:fs');
const path = require('node:path');
const assert = require('node:assert/strict');

const root = path.resolve(__dirname, '..');
const html = fs.readFileSync(path.join(root, 'xiaozhi_core/web/skills.html'), 'utf8');
const output = path.resolve(process.env.QZ_WEB_LAYOUT_OUTPUT || path.join(root, 'build/web-layout-check'));
const skills = Array.from({ length: 12 }, (_, i) => ({
    id: `layout-${i}`, name: '测试技能名称' + 'W'.repeat(40),
    description: '完整说明用于检查小屏文字换行。'.repeat(20),
    role: i ? 'secondary' : 'primary', writable: true
}));
const fixture = {
    skills, summary: { primary: 1, secondary: 11 },
    cpu: { usage_percent: 98.7, level: 'critical' },
    memory: { usage_percent: 92.5, level: 'critical' },
    storage: { usage_percent: 88.8, level: 'warn' },
    temperature: { celsius: 99.9, level: 'critical' },
    wifi: { signal_dbm: -99, level: 'warn' }, network: { latency_ms: 999.9 },
    services: { qzdesk_core: 'running' }, uptime_secs: 123456789,
    city: '测试城市'.repeat(10), available: false,
    error: '网络暂不可用，请稍后重试。'.repeat(8),
    messages: [{ id: 1, role: 'assistant', text: '长中文回复。'.repeat(60) },
               { id: 2, role: 'user', text: 'W'.repeat(300) }]
};

async function assertWindow(page, view) {
    const result = await page.evaluate(() => {
        const panel = document.querySelector('.compact-active');
        const rect = panel.getBoundingClientRect();
        return {
            width: innerWidth, height: innerHeight,
            rootWidth: document.documentElement.scrollWidth,
            rootHeight: document.documentElement.scrollHeight,
            panelWidth: panel.clientWidth, contentWidth: panel.scrollWidth,
            left: rect.left, top: rect.top, right: rect.right, bottom: rect.bottom,
            count: document.querySelectorAll('.compact-active').length
        };
    });
    assert.equal(result.count, 1, view + ': one visible panel');
    assert.ok(result.rootWidth <= result.width, view + ': horizontal page overflow');
    assert.ok(result.rootHeight <= result.height, view + ': vertical page overflow');
    assert.ok(result.contentWidth <= result.panelWidth + 1, view + ': horizontal content overflow');
    assert.ok(result.left >= 0 && result.top >= 0 && result.right <= result.width &&
              result.bottom <= result.height, view + ': panel bounds');
}

(async () => {
    fs.mkdirSync(output, { recursive: true });
    const browser = await chromium.launch({ headless: true });
    try {
        for (const [width, height] of [[320, 240], [480, 320], [320, 320]]) {
            const context = await browser.newContext({ viewport: { width, height }, hasTouch: true,
                                                       reducedMotion: 'reduce' });
            const page = await context.newPage();
            const errors = [];
            page.on('pageerror', error => errors.push(error.message));
            await page.route('**/*', route => {
                const url = new URL(route.request().url());
                if (url.pathname === '/') return route.fulfill({ contentType: 'text/html', body: html });
                const data = url.pathname.endsWith('/content')
                    ? { ok: true, content: '# 测试技能\n' + '完整内容。'.repeat(100) } : fixture;
                return route.fulfill({ contentType: 'application/json', body: JSON.stringify(data) });
            });
            await page.goto('http://qzdesk-layout.test/');
            await page.waitForSelector('.skill');
            for (const view of ['skills', 'chat', 'editor', 'stats', 'weather', 'performance', 'guide']) {
                await page.selectOption('#compactView', view);
                if (view === 'editor') await page.locator('#editorBox summary').click();
                await assertWindow(page, view);
                await page.screenshot({ path: path.join(output, `${width}x${height}-${view}.png`) });
            }
            await page.selectOption('#compactView', 'skills');
            await page.locator('[data-act="edit"]').first().click();
            await page.waitForFunction(() => document.querySelector('#compactView').value === 'editor');
            await assertWindow(page, 'edit-skill');
            await page.locator('#saveBtn').scrollIntoViewIfNeeded();
            assert.ok(await page.locator('#saveBtn').isVisible(), 'save action remains reachable');
            await page.selectOption('#compactView', 'chat');
            await page.locator('#chatInput').fill('完整输入内容。'.repeat(60));
            await assertWindow(page, 'chat-input');
            await page.locator('#lgToggle').click();
            const bounds = await page.locator('.lg-content').boundingBox();
            assert.ok(bounds && bounds.x >= 0 && bounds.y >= 0 &&
                      bounds.x + bounds.width <= width && bounds.y + bounds.height <= height,
                      'floating card remains inside viewport');
            await page.locator('.lg-close').click();
            assert.deepEqual(errors, [], 'browser runtime errors');
            console.log(`WEB ${width}x${height}: 7 views, editor, long chat, floating card passed`);
            await context.close();
        }
    } finally {
        await browser.close();
    }
})().catch(error => { console.error(error); process.exitCode = 1; });
