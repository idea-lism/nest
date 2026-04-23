module.exports = grammar({
  name: 'calc',

  extras: $ => [/\s+/],

  rules: {
    source_file: $ => $.expression,

    expression: $ => choice(
      prec.left(1, seq($.expression, '+', $.term)),
      prec.left(1, seq($.expression, '-', $.term)),
      $.term,
    ),

    term: $ => choice(
      prec.left(2, seq($.term, '*', $.factor)),
      prec.left(2, seq($.term, '/', $.factor)),
      $.factor,
    ),

    factor: $ => choice(
      $.number,
      seq('(', $.expression, ')'),
    ),

    number: _ => /[0-9]+/,
  },
});
