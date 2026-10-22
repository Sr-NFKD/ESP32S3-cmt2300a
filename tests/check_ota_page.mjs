// Exercise the real embedded page script against the actual built app image.
import fs from 'node:fs';
import vm from 'node:vm';
import assert from 'node:assert/strict';

const html = fs.readFileSync(new URL('../components/debug_ota/web/index.html', import.meta.url), 'utf8');
const script = html.match(/<script>([\s\S]*?)<\/script>/)[1];
const image = fs.readFileSync(new URL('../build/ESP32S3_A07_CC1101.bin', import.meta.url));
const nodes = new Map();
const downloads=[];
function node(tag='div') {
  return {tag,textContent:'',className:'',disabled:false,checked:false,files:[],value:'',children:[],
    events:{},addEventListener(type, callback){this.events[type]=callback;},
    append(...items){this.children.push(...items);},replaceChildren(){this.children=[];},
    setAttribute(){},click(){if(this.tag==='a')downloads.push(this);}};
}
for (const id of html.matchAll(/id="([^"]+)"/g))nodes.set(id[1],node());
nodes.get('retain-data').checked=true;
assert.match(html, /id="retain-data"[^>]*checked/);
assert.doesNotMatch(html, /id="reset-settings"[^>]*checked/);
let saved=[{id:1,name:'客厅灯',key_code:'0012AB',frequency_hz:433920000,protocol:'test'},
  {id:2,name:'<img src=x onerror=alert(1)>',key_code:'00FF'}];
let mutationFailed=false;
const calls=[];
const response=(data,ok=true)=>({ok,json:async()=>structuredClone(data),text:async()=>'Write failed'});
let sent;
class Request {
  constructor(){this.upload={};this.headers={};}
  open(method,url){this.method=method;this.url=url;}
  setRequestHeader(name,value){this.headers[name]=value;}
  send(body){this.body=body;sent=this;}
}
const context=vm.createContext({
  document:{getElementById:id=>nodes.get(id),createElement:node},
  fetch:async(url,options)=>{
    calls.push({url,options});
    if(url==='/api/info')return response({project:'ESP32S3_A07_CC1101',version:'1',chip:'ESP32-S3',max_size:0x1e0000});
    if(url==='/api/signals')return response(saved);
    assert.equal(options.headers['X-RF-API'],'1');
    if(mutationFailed)return response({},false);
    const data=JSON.parse(options.body);
    if(url.endsWith('/update'))Object.assign(saved.find(record=>record.id===data.id),data);
    else saved=saved.filter(record=>record.id!==data.id);
    return response({ok:true});
  },
  Blob,URL:{createObjectURL:blob=>{downloads.blob=blob;return 'blob:test';},revokeObjectURL(){}},
  setTimeout:callback=>callback(),TextEncoder,
  window:{confirm:()=>true},XMLHttpRequest:Request,Uint8Array,DataView,TextDecoder,
});
vm.runInContext(script,context);
await new Promise(setImmediate);
assert.equal(nodes.get('count').textContent,2);
const cards=()=>nodes.get('signals').children;
assert.equal(cards()[1].children[1].children[0].children[0].value,saved[1].name,'Names are plain text');
cards()[0].children[1].children[0].children[0].value='门铃';
cards()[0].children[1].children[1].children[0].value='0000ABCD';
cards()[0].children[2].children[0].events.click();await new Promise(setImmediate);
assert.equal(saved[0].name,'门铃');assert.equal(saved[0].key_code,'0000ABCD');
assert.equal(saved[0].protocol,'test','Editing must preserve RF metadata');
cards()[0].children[1].children[1].children[0].value='not-hex';
const before=calls.length;cards()[0].children[2].children[0].events.click();
assert.equal(calls.length,before,'Invalid key must not reach device');
cards()[0].children[2].children[1].events.click();
let exported=JSON.parse(await downloads.blob.text());
assert.equal(exported.signals[0].key_code,'0000ABCD');assert.equal(exported.signals.length,1);
nodes.get('select-all').events.change({target:{checked:true}});
nodes.get('export-selected').events.click();
exported=JSON.parse(await downloads.blob.text());assert.equal(exported.signals.length,2);
cards()[0].children[0].children[0].checked=false;cards()[0].children[0].children[0].events.change();
nodes.get('export-selected').events.click();exported=JSON.parse(await downloads.blob.text());
assert.equal(exported.signals.length,1);assert.equal(exported.signals[0].id,2);
mutationFailed=true;cards()[0].children[2].children[2].events.click();await new Promise(setImmediate);
assert.equal(saved.length,2,'Failed delete must retain signal');
mutationFailed=false;cards()[1].children[2].children[2].events.click();await new Promise(setImmediate);
assert.equal(saved.length,1);assert.equal(nodes.get('export-selected').disabled,true);
nodes.get('export-all').events.click();exported=JSON.parse(await downloads.blob.text());assert.equal(exported.signals.length,1);
saved.push(...[0x55,0x11,0x33].map((button_code,i)=>({id:8+i,name:'RAW',key_code:'0',protocol:'RAW',button_code})));
await vm.runInContext('loadSignals()',context);
for(let i=0;i<3;i++){
  const label=cards()[i+1].children[1].children[1],input=label.children[0];
  assert.equal(input.value,['55','11','33'][i]);assert.equal(input.readOnly,false);
  assert.equal(input.maxLength,2);assert.ok(!/[开关停]/.test(label.textContent));
}
cards()[1].children[1].children[1].children[0].value='A7';
cards()[1].children[2].children[0].events.click();await new Promise(setImmediate);
assert.equal(saved[1].key_code,'A7','Suffix is not restricted to 11/33/55');
saved=saved.slice(0,1);await vm.runInContext('loadSignals()',context);
function file(data,name='ESP32S3_A07_CC1101.bin') {
  return {name,size:data.length,slice(start,end){const bytes=data.subarray(start,end);
    return {arrayBuffer:async()=>bytes.buffer.slice(bytes.byteOffset,bytes.byteOffset+bytes.byteLength)};}};
}
async function selectAndUpload(selected) {
  nodes.get('firmware').files=[selected];
  nodes.get('firmware').events.change();
  await nodes.get('upload').events.click();
}
const wrongChip=Buffer.from(image);wrongChip[12]=0;
await selectAndUpload(file(wrongChip));
assert.equal(sent,undefined,'Wrong-chip image must not be sent');
const wrongProject=Buffer.from(image);wrongProject[80]=88;
await selectAndUpload(file(wrongProject));
assert.equal(sent,undefined,'Wrong-project image must not be sent');
const bootloader=fs.readFileSync(new URL('../build/bootloader/bootloader.bin',import.meta.url));
await selectAndUpload(file(bootloader,'bootloader.bin'));
assert.equal(sent,undefined,'Bootloader must not be sent');
await selectAndUpload(file(Buffer.alloc(0x1e0001)));
assert.equal(sent,undefined,'Oversized image must not be sent');
const valid=file(image);
await selectAndUpload(valid);
assert.equal(sent.method,'POST');assert.equal(sent.url,'/api/ota');
assert.equal(sent.body,valid,'Send raw application bytes, not multipart');
assert.equal(sent.headers['Content-Type'],'application/octet-stream');
assert.equal(sent.headers['X-RF-OTA'],'1');
assert.equal(sent.headers['X-RF-Retain'],'1');assert.equal(sent.headers['X-RF-Reset'],'0');
assert.equal(cards()[0].children[2].children[0].disabled,true,'No edits during upgrade');
sent.upload.onprogress({lengthComputable:true,loaded:50,total:100});
assert.equal(nodes.get('progress').value,50);
sent.status=400;sent.responseText='Wrong image';sent.onload();
assert.equal(nodes.get('upload').disabled,false,'Allow retry after failure');
nodes.get('retain-data').checked=false;nodes.get('reset-settings').checked=true;
await selectAndUpload(valid);
assert.equal(sent.headers['X-RF-Retain'],'0');assert.equal(sent.headers['X-RF-Reset'],'1');
sent.status=200;sent.responseText=JSON.stringify({ok:true,message:'Restarting'});sent.onload();
assert.equal(nodes.get('status').textContent,'Restarting');
assert.equal(nodes.get('progress').value,100);
console.log('PASS: signal editing/validation/delete/export, metadata, selections, upgrade flags, actual image validation and retries');
