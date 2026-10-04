(() => {
  const dom = [], boxes = [], textGeometry = [];
  function walk(node, path) {
    const attrs = {};
    if (node.nodeType === 1) Array.from(node.attributes).sort((a,b) => a.name.localeCompare(b.name)).forEach(a => attrs[a.name] = a.value);
    dom.push({path,type:node.nodeType,tag:node.nodeType === 1 ? node.localName : '',text:node.nodeType === 1 ? '' : node.nodeValue || '',attrs});
    if (node.nodeType === 3 && node.parentElement && !node.parentElement.closest('head,script,style,option')) {
      const s = getComputedStyle(node.parentElement);
      // Read-only DOM ranges retain UTF-16 source offsets across wrapping and
      // collapsed spaces. Spaces are recorded by their effect on glyph positions.
      for (let start = 0; start < node.length;) {
        const code = node.nodeValue.codePointAt(start), length = code > 0xffff ? 2 : 1;
        const text = node.nodeValue.slice(start, start + length);
        if (!/^[ \t\r\n\f]+$/.test(text)) {
          const range = document.createRange();
          range.setStart(node, start); range.setEnd(node, start + length);
          for (const r of range.getClientRects()) {
            if (r.width || r.height) textGeometry.push({path,start,length,text,mapped:true,rect:[r.x,r.y,r.width,r.height],declaredFont:s.fontFamily});
          }
          range.detach();
        }
        start += length;
      }
    }
    if (node.nodeType === 1 && node.hasAttribute('data-case-node')) {
      const r = node.getBoundingClientRect(), s = getComputedStyle(node), styles = {};
      const properties = new Set([...Array.from(s),'white-space','font','background','border','margin','padding']);
      properties.forEach(p => styles[p] = s.getPropertyValue(p));
      const computedStyles = {};
      'display position visibility box-sizing white-space direction text-align font-style font-weight font-kerning pointer-events overflow-x overflow-y flex-direction flex-wrap flex-grow flex-shrink list-style-position list-style-type opacity letter-spacing line-height font-size color background-color'.split(' ').forEach(p => computedStyles[p] = s.getPropertyValue(p));
      boxes.push({id:node.getAttribute('data-case-node'),present:node.getClientRects().length > 0,rect:[r.x,r.y,r.width,r.height],scroll:[node.scrollWidth,node.scrollHeight],styles,computedStyles});
    }
    Array.from(node.childNodes).forEach((n,i) => walk(n,path+'/'+i));
  }
  walk(document.documentElement,'0');
  return {captureRoute:'webview2-capture-preview',styleContractVersion:1,width:innerWidth,height:innerHeight,dpr:devicePixelRatio,dom,boxes,textGeometry};
})();
