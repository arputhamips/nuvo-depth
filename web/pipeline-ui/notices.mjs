import {readFileSync,writeFileSync,existsSync} from 'node:fs';
const base=new URL('./',import.meta.url),lock=JSON.parse(readFileSync(new URL('package-lock.json',base),'utf8'));
const sections=['Nuvo pipeline browser bundle — production dependency notices\n'];
for(const [name,pkg] of Object.entries(lock.packages)){
  if(!name||pkg.dev)continue;
  const license=['LICENSE','LICENSE.md','LICENSE.txt'].map(file=>new URL(name+'/'+file,base)).find(existsSync);
  sections.push('\n--- '+name+' '+pkg.version+' ---\n'+(license?readFileSync(license,'utf8'):'License: '+pkg.license)+'\n');
}
writeFileSync(new URL('../pipeline-licenses.txt',base),sections.join('').trimEnd()+'\n');
