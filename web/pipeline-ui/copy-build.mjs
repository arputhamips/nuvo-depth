import {copyFileSync} from 'node:fs';
import './notices.mjs';
for(const file of ['pipeline-canvas.js','pipeline-canvas.css'])copyFileSync(new URL('./dist/'+file,import.meta.url),new URL('../'+file,import.meta.url));
