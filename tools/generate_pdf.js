const puppeteer = require('puppeteer');
const path = require('path');

(async () => {
    const htmlPath = path.resolve(__dirname, '..', 'docs', 'user_manual.html');
    const outputPath = path.resolve(__dirname, '..', 'docs', 'Pico_Universal_Programmer_User_Manual.pdf');

    console.log('[*] Launching browser...');
    const browser = await puppeteer.launch({ headless: 'shell' });
    const page = await browser.newPage();

    console.log('[*] Loading HTML:', htmlPath);
    await page.goto('file:///' + htmlPath.replace(/\\/g, '/'), { waitUntil: 'networkidle0', timeout: 30000 });

    console.log('[*] Rendering PDF...');
    await page.pdf({
        path: outputPath,
        format: 'A4',
        printBackground: true,
        margin: { top: '0', right: '0', bottom: '0', left: '0' },
        displayHeaderFooter: false,
    });

    console.log('[SUCCESS] PDF saved to:', outputPath);
    await browser.close();
})();
