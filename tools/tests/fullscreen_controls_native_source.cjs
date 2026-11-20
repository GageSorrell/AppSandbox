const fs=require('node:fs'),path=require('node:path');
const root=path.resolve(__dirname,'../..');
const source=fs.readFileSync(path.join(root,'src/backend_win/agent_overlay.c'),'utf8');
const names=['active','menu_visible','layout_rect','layout','agent_overlay_hit_test','agent_overlay_cancel_input','agent_overlay_set_pinned','agent_overlay_set_fullscreen'];
const functions=names.map(name=>{
 const re=new RegExp('^(?:static )?(?:void|BOOL) '+name+'\\([^]*?^}', 'm');
 const match=source.match(re);if(!match)throw Error('Missing function '+name);return match[0];
}).join('\n');
const template=fs.readFileSync(path.join(__dirname,'fullscreen_controls_native_test.c'),'utf8');
fs.writeFileSync(process.argv[2],template.replace('/* ACTUAL OVERLAY FUNCTIONS */',functions));
