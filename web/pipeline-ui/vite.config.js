import {defineConfig} from 'vite';
export default defineConfig({define:{'process.env.NODE_ENV':'"production"'},build:{outDir:'dist',lib:{entry:'src/canvas.jsx',name:'NuvoPipelineCanvas',formats:['iife'],fileName:()=> 'pipeline-canvas.js'},cssCodeSplit:false,rollupOptions:{output:{assetFileNames:'pipeline-canvas.[ext]'}}}});
