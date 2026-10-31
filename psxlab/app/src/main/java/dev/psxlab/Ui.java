package dev.psxlab;
import android.app.*;
import android.content.res.ColorStateList;
import android.graphics.Color;
import android.graphics.Typeface;
import android.graphics.Path;
import android.graphics.drawable.*;
import android.graphics.drawable.shapes.PathShape;
import android.view.*;
import android.widget.*;

final class Ui {
    static final int BG=0xff0d111a,PANEL=0xff171e2b,TEXT=0xffedf2fc,MUTED=0xff92a1b8,ACCENT=0xff82e5cf;
    static String version(android.content.Context c){try{return c.getPackageManager().getPackageInfo(c.getPackageName(),0).versionName.split("-",2)[0];}catch(android.content.pm.PackageManager.NameNotFoundException e){return "Development build";}}
    static int dp(android.content.Context c,float n){return Math.round(n*c.getResources().getDisplayMetrics().density);}
    static GradientDrawable box(int color,float radius,android.content.Context c){GradientDrawable d=new GradientDrawable();d.setColor(color);d.setCornerRadius(dp(c,radius));return d;}
    static GradientDrawable border(int fill,int line,float radius,android.content.Context c){GradientDrawable d=box(fill,radius,c);d.setStroke(dp(c,1),line);return d;}
    static Drawable outline(android.content.Context c,float radius,boolean selected){StateListDrawable d=new StateListDrawable();GradientDrawable pressed=border(Color.TRANSPARENT,ACCENT,radius,c);pressed.setStroke(dp(c,2),ACCENT);GradientDrawable focused=border(Color.TRANSPARENT,ACCENT,radius,c);focused.setStroke(dp(c,2),ACCENT);d.addState(new int[]{android.R.attr.state_pressed},pressed);d.addState(new int[]{android.R.attr.state_focused},focused);d.addState(new int[]{},border(Color.TRANSPARENT,selected?ACCENT:MUTED,radius,c));return d;}
    static void selectOutline(Button b,boolean selected){b.setSelected(selected);b.setTextColor(new ColorStateList(new int[][]{new int[]{android.R.attr.state_focused},new int[]{android.R.attr.state_pressed},new int[]{}},new int[]{ACCENT,ACCENT,TEXT}));b.setBackground(outline(b.getContext(),10,false));}
    static Button outlineButton(android.content.Context c,String label,boolean selected){Button b=button(c,label,false);b.setDefaultFocusHighlightEnabled(false);selectOutline(b,selected);return b;}
    static Drawable optionBackground(android.content.Context c){StateListDrawable d=new StateListDrawable();for(int state:new int[]{android.R.attr.state_selected,android.R.attr.state_focused,android.R.attr.state_pressed}){GradientDrawable line=border(PANEL,ACCENT,6,c);line.setStroke(dp(c,2),ACCENT);d.addState(new int[]{state},line);}d.addState(new int[]{},box(PANEL,6,c));return d;}
    static TextView text(android.content.Context c,String value,int size,int color){TextView t=new TextView(c);t.setText(value);t.setTextSize(size);t.setTextColor(color);return t;}
    static TextView heading(android.content.Context c,String value,int size){TextView t=text(c,value,size,TEXT);t.setTypeface(Typeface.DEFAULT,Typeface.BOLD);return t;}
    static Button button(android.content.Context c,String label,boolean primary){Button b=new Button(c);b.setText(label);b.setAllCaps(false);b.setTextSize(14);b.setTextColor(primary?BG:TEXT);StateListDrawable background=new StateListDrawable();GradientDrawable focused=border(primary?ACCENT:PANEL,primary?TEXT:ACCENT,10,c);focused.setStroke(dp(c,2),primary?TEXT:ACCENT);background.addState(new int[]{android.R.attr.state_focused},focused);background.addState(new int[]{},box(primary?ACCENT:PANEL,10,c));b.setBackground(background);b.setDefaultFocusHighlightEnabled(false);b.setMinHeight(dp(c,44));b.setPadding(dp(c,16),0,dp(c,16),0);return b;}
    static void chrome(Activity a){a.getWindow().setStatusBarColor(BG);a.getWindow().setNavigationBarColor(BG);a.getWindow().getDecorView().setSystemUiVisibility(0);}
    static LinearLayout page(Activity a,String title){chrome(a);LinearLayout outer=new LinearLayout(a);outer.setOrientation(1);outer.setBackgroundColor(BG);outer.setPadding(dp(a,24),dp(a,12),dp(a,24),dp(a,16));LinearLayout bar=new LinearLayout(a);bar.setGravity(Gravity.CENTER_VERTICAL);Button back=button(a,"‹",false);back.setContentDescription("Back");back.setOnClickListener(v->a.finish());bar.addView(back,new LinearLayout.LayoutParams(dp(a,48),dp(a,42)));TextView name=heading(a,title,24);name.setPadding(dp(a,16),0,0,0);bar.addView(name);outer.addView(bar);ScrollView scroll=new ScrollView(a);scroll.setFillViewport(true);LinearLayout content=new LinearLayout(a);content.setOrientation(1);content.setPadding(0,dp(a,20),0,0);scroll.addView(content);outer.addView(scroll,new LinearLayout.LayoutParams(-1,0,1));a.setContentView(outer);return content;}
    static void gap(LinearLayout p,int n){View v=new View(p.getContext());p.addView(v,new LinearLayout.LayoutParams(1,dp(p.getContext(),n)));}
    static Spinner choice(Activity a,LinearLayout p,String title,String[] items,int selected,java.util.function.IntConsumer change){
        TextView label=text(a,title,12,MUTED);p.addView(label);gap(p,5);Spinner spinner=new DropdownSpinner(a);spinner.setBackgroundTintList(null);spinner.setDefaultFocusHighlightEnabled(false);
        Path path=new Path();path.moveTo(0,0);path.lineTo(10,0);path.lineTo(5,6);path.close();ShapeDrawable arrow=new ShapeDrawable(new PathShape(path,10,6));arrow.getPaint().setColor(ACCENT);LayerDrawable field=new LayerDrawable(new Drawable[]{outline(a,8,false),arrow});field.setLayerSize(1,dp(a,10),dp(a,6));field.setLayerGravity(1,Gravity.RIGHT|Gravity.CENTER_VERTICAL);field.setLayerInsetRight(1,dp(a,14));spinner.setBackground(field);
        ArrayAdapter<String> adapter=new ArrayAdapter<String>(a,android.R.layout.simple_spinner_item,new java.util.ArrayList<>(java.util.Arrays.asList(items))){
            private View row(int position,View old){TextView value=old instanceof TextView?(TextView)old:text(a,"",14,TEXT);value.setText(getItem(position));value.setTextColor(TEXT);value.setTextSize(14);value.setGravity(Gravity.CENTER_VERTICAL);value.setPadding(dp(a,14),dp(a,8),dp(a,46),dp(a,8));value.setMinHeight(dp(a,48));value.setMaxLines(2);value.setEllipsize(android.text.TextUtils.TruncateAt.END);return value;}
            @Override public View getView(int at,View old,ViewGroup parent){View v=row(at,old);v.setBackgroundColor(Color.TRANSPARENT);return v;}
            @Override public View getDropDownView(int at,View old,ViewGroup parent){View v=row(at,old);v.setBackground(optionBackground(a));return v;}
        };
        spinner.setAdapter(adapter);spinner.setSelection(selected,false);int[] current={selected};spinner.setOnItemSelectedListener(new AdapterView.OnItemSelectedListener(){public void onNothingSelected(AdapterView<?> parent){}public void onItemSelected(AdapterView<?> parent,View view,int at,long id){if(at!=current[0]){current[0]=at;change.accept(at);}}});p.addView(spinner,new LinearLayout.LayoutParams(-1,-2));gap(p,12);return spinner;
    }
    static void toggle(Activity a,LinearLayout p,String title,String detail,boolean value,java.util.function.Consumer<Boolean> change){choice(a,p,title,new String[]{"Off","On"},value?1:0,which->change.accept(which==1));TextView help=text(a,detail,12,MUTED);p.addView(help);gap(p,12);}
}
