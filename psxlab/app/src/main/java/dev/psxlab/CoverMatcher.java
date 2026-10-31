package dev.psxlab;

import java.text.Normalizer;
import java.util.*;
import java.util.regex.*;

/** Conservative title lookup; ambiguous names fall through to the disc serial. */
final class CoverMatcher {
    private final Map<String,List<String>> titles=new HashMap<>();
    CoverMatcher(Collection<String> names){for(String name:names)titles.computeIfAbsent(normalize(name),k->new ArrayList<>()).add(name);}
    static String normalize(String name){
        String s=Normalizer.normalize(name,Normalizer.Form.NFKD).replaceAll("\\p{M}+","").toLowerCase(Locale.ROOT)
            .replaceFirst("\\.(png|jpg|jpeg|webp|chd|bin|iso|img|exe)$","")
            .replaceAll("\\([^)]*\\)|\\[[^]]*\\]", " ").replace("&"," and ")
            .replaceAll("['’]", "").replaceAll("[^a-z0-9]+", " ").trim();
        s=s.replaceFirst("^(the|an|a) +", "").replaceFirst(" +(the|an|a)$", "");
        String[] roman={"i","ii","iii","iv","v","vi","vii","viii","ix"};
        for(int i=0;i<roman.length;i++)s=s.replaceAll("\\b"+roman[i]+"\\b",Integer.toString(i+1));
        s=s.replaceAll("\\s+"," ");
        if(s.equals("crash bandicoot warped"))s="crash bandicoot 3 warped";
        return s;
    }
    private static String numbers(String s){return s.replaceAll("[^0-9]+"," ").trim();}
    List<String> find(String filename){
        String key=normalize(filename);List<String> found=titles.get(key);
        if(found==null && key.length()>=8 && key.contains(" ")){
            String only=null;boolean ambiguous=false;
            for(String candidate:titles.keySet())if(candidate.startsWith(key+" ") && numbers(candidate).equals(numbers(key))){if(only!=null){ambiguous=true;break;}only=candidate;}
            if(!ambiguous && only!=null)found=titles.get(only);
        }
        if(found==null && key.length()>=9){
            int best=3;String only=null;boolean ambiguous=false;
            for(String candidate:titles.keySet()){
                if(!numbers(candidate).equals(numbers(key)) || !candidate.split(" ",2)[0].equals(key.split(" ",2)[0]))continue;
                int d=distance(key,candidate);
                if(d>2 || d*10>Math.max(key.length(),candidate.length()))continue;
                if(d<best){best=d;only=candidate;ambiguous=false;}else if(d==best)ambiguous=true;
            }
            if(only!=null && !ambiguous)found=titles.get(only);
        }
        if(found==null)return Collections.emptyList();
        ArrayList<String> result=new ArrayList<>(found);String lower=filename.toLowerCase(Locale.ROOT);
        result.sort(Comparator.<String>comparingInt(n->rank(n,lower)).reversed().thenComparing(Comparator.naturalOrder()));
        return result.subList(0,Math.min(3,result.size()));
    }
    private static int rank(String name,String requested){
        String n=name.toLowerCase(Locale.ROOT);int score=n.contains("(usa)")?30:n.contains("(europe)")?20:0;
        for(String region:new String[]{"usa","europe","japan","world"})if(requested.contains("("+region+")") && n.contains("("+region+")"))score+=100;
        for(String tag:new String[]{"beta","proto","demo","sample"})if(n.contains("("+tag) && !requested.contains(tag))score-=200;
        if(name.equalsIgnoreCase(requested.replaceFirst("(?i)\\.(chd|iso|bin|img|exe)$", ".png")))score+=1000;
        return score;
    }
    private static int distance(String a,String b){
        if(Math.abs(a.length()-b.length())>2)return 3;
        int[] row=new int[b.length()+1];for(int j=0;j<row.length;j++)row[j]=j;
        for(int i=1;i<=a.length();i++){int diagonal=row[0];row[0]=i;for(int j=1;j<=b.length();j++){int old=row[j];row[j]=Math.min(Math.min(row[j]+1,row[j-1]+1),diagonal+(a.charAt(i-1)==b.charAt(j-1)?0:1));diagonal=old;}}
        return row[b.length()];
    }
    static String serial(String text){
        Matcher m=Pattern.compile("(?i)(?<![a-z0-9])([a-z]{4})[-_ ]?(\\d{3})[.\\- ]?(\\d{2})(?!\\d)").matcher(text);
        while(m.find()){String prefix=m.group(1).toUpperCase(Locale.ROOT);if(Arrays.asList("SCUS","SCES","SCPS","SLUS","SLES","SLPS","SLPM","PAPX","PCPX","PBPX","SCED","SLED","SIPS").contains(prefix))return prefix+"-"+m.group(2)+m.group(3);}
        return null;
    }
}
