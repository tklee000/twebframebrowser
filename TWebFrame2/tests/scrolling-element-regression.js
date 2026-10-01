var expectedScrollingElement = document.compatMode === 'CSS1Compat' ?
    document.documentElement : document.body;
if (document.scrollingElement !== expectedScrollingElement)
    throw new Error('scrollingElement must follow the document mode');
document.documentElement.scrollTop = 37;
document.documentElement.scrollLeft = 11;
if (document.body.scrollTop !== 37 || document.body.scrollLeft !== 11)
    throw new Error('root and body must share viewport scroll state');
document.documentElement.scrollBy(4, 5);
if (document.documentElement.scrollTop !== 42 || document.body.scrollLeft !== 15)
    throw new Error('root scrolling methods must update viewport scroll state');
'scrolling-element-ok';
