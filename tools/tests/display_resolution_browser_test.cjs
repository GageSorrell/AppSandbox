const assert = require('node:assert/strict');
const path = require('node:path');
const {pathToFileURL} = require('node:url');
const repo = path.resolve(__dirname,'../..');
const {chromium} = require(path.join(repo,'bin/tests/ui-deps/node_modules/playwright-core'));
(async () => {
    const browser = await chromium.launch({headless:true,executablePath:process.env.ASB_TEST_BROWSER || 'C:\\Program Files (x86)\\Microsoft\\Edge\\Application\\msedge.exe'});
    try {
        const page = await browser.newPage({viewport:{width:1024,height:900}});
        const errors=[];page.on('pageerror',e=>errors.push(e.message));
        await page.addInitScript(() => {
            window.__commands=[];
            window.chrome=window.chrome || {};
            window.chrome.webview={postMessage(message){window.__commands.push(message);},addEventListener(){}};
        });
        await page.goto(pathToFileURL(path.join(repo,'web/index.html')).href);
        await page.evaluate(() => {
            window.onHostMessage({type:'fullState',vms:[{id:'vm-1',name:'Windows 11',osType:'windows11',osName:'Windows 11',ramMb:8192,cpuCores:4,gpuMode:0,gpuId:'',hddGb:64,networkMode:1,displaySettingsError:0,
                displayResolution:{windowedWidth:5120,windowedHeight:2880,fullscreenOverride:true,fullscreenWidth:640,fullscreenHeight:480}}]});
        });
        await page.locator('button.edit').click();
        await page.evaluate(() => window.onHostMessage({type:'displayCapabilities',ok:true,errorCode:0,
            monitors:[{device:'ultrawide',modes:[{width:3840,height:1600}]}],
            presets:[{width:640,height:480},{width:800,height:600},{width:1280,height:720},{width:1920,height:1080},{width:2560,height:1080},{width:3840,height:1600}]}));
        assert.equal(await page.locator('#edit-windowed-preset').inputValue(),'custom');
        assert.equal(await page.locator('#edit-windowed-width').inputValue(),'5120');
        assert(await page.locator('#edit-windowed-limit').isVisible());
        assert(await page.locator('#edit-fullscreen-small').isVisible());
        assert(await page.locator('#btn-save-edit-vm').isEnabled());
        await page.screenshot({path:path.join(repo,'bin/tests/resolution-editor.png')});
        await page.locator('#edit-windowed-width').fill('2048');
        await page.locator('#edit-windowed-height').fill('1152');
        await page.locator('#edit-fullscreen-preset').selectOption('custom');
        await page.locator('#edit-fullscreen-width').fill('');
        assert(await page.locator('#btn-save-edit-vm').isDisabled());
        await page.locator('#edit-fullscreen-override').uncheck();
        assert(await page.locator('#btn-save-edit-vm').isEnabled());
        await page.locator('#btn-save-edit-vm').click();
        const message=await page.evaluate(()=>window.__commands.filter(m=>m.action==='setDisplayResolution').at(-1));
        assert.equal(message.resolution.windowedWidth,2048);assert.equal(message.resolution.windowedHeight,1152);
        assert.equal(message.resolution.fullscreenWidth,640);assert.equal(message.resolution.fullscreenHeight,480);
        assert.equal(message.resolution.fullscreenOverride,false);
        // A small viewport keeps Save reachable in the existing scrolling modal.
        await page.setViewportSize({width:640,height:420});
        await page.locator('button.edit').click();
        await page.evaluate(()=>window.onHostMessage({type:'displayCapabilities',ok:false,errorCode:-1,monitors:[],presets:[]}));
        await page.locator('#btn-save-edit-vm').scrollIntoViewIfNeeded();
        const box=await page.locator('#btn-save-edit-vm').boundingBox();assert(box && box.y>=0 && box.y+box.height<=420);
        await page.screenshot({path:path.join(repo,'bin/tests/resolution-editor-small.png')});
        assert.deepEqual(errors,[]);
        console.log('PASS headless Edge: rendered editor, inline warnings, hidden invalid fields, one resolution save, small-window layout');
    } finally {await browser.close();}
})().catch(error=>{console.error(error);process.exitCode=1;});
